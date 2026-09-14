#include "EpollTcpServer.h"
#include "def.h"
#include "../mediator/iNetMediator.h"
#include "../common/Logger.h"
#include "../common/ServerMetrics.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

namespace {
// 一次 epoll_wait 最多取出的事件数。
constexpr int kMaxEvents = 256;                
// 单连接待发送数据上限，防慢客户端耗尽内存。
constexpr size_t kMaxQueuedOutput = 8 * 1024 * 1024;
// 每 10 秒检查一次，连续 90 秒没有收到数据则认为连接失效。
constexpr int kIdleCheckSeconds = 10;
constexpr int kConnectionTimeoutSeconds = 90;
// HTTP请求头只需包含请求行和少量通用字段，限制大小防止恶意占用内存。
constexpr size_t kMaxMetricsRequestSize = 8 * 1024;

// 从环境变量读取有上下限的正整数配置，非法值回退到默认值。
size_t readSizeSetting(const char* name, size_t defaultValue, size_t maximum) {
    const char* text = std::getenv(name);
    if (text == nullptr) return defaultValue;
    char* end = nullptr;
    errno = 0;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0 || value > maximum) {
        return defaultValue;
    }
    return static_cast<size_t>(value);
}
}

EpollTcpServer::EpollTcpServer(INetMediator* mediator) {
    m_pMediator = mediator;
    m_isRunning = false;
    m_workerCount = readSizeSetting("IM_WORKER_THREADS", 4, 64);
    m_maxPendingJobs = readSizeSetting("IM_MAX_PENDING_JOBS", 10000, 1000000);
    m_metricsPort = static_cast<uint16_t>(
        readSizeSetting("IM_METRICS_PORT", 9108, 65535));
}
EpollTcpServer::~EpollTcpServer() { unInitNet(); }

bool EpollTcpServer::setNonBlocking(int fd) const {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags != -1 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

bool EpollTcpServer::initNet() {
    if (m_reactorThread.joinable()) return true;
    // 监听 socket 从创建开始就是非阻塞，并在 exec 时自动关闭。
    m_sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (m_sock == INVALID_SOCKET) { perror("socket"); return false; }
    int reuse = 1;
    setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(DEF_TCP_PORT);
    if (bind(m_sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ||
        listen(m_sock, SOMAXCONN)) {
        perror("bind/listen"); close(m_sock); m_sock = INVALID_SOCKET; return false;
    }

    // 监控端点也使用非阻塞socket，并由同一个epoll Reactor管理。
    m_metricsSock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (m_metricsSock < 0) {
        perror("metrics socket"); unInitNet(); return false;
    }
    setsockopt(m_metricsSock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in metricsAddress{};
    metricsAddress.sin_family = AF_INET;
    metricsAddress.sin_addr.s_addr = htonl(INADDR_ANY);
    metricsAddress.sin_port = htons(m_metricsPort);
    if (bind(m_metricsSock, reinterpret_cast<sockaddr*>(&metricsAddress),
        sizeof(metricsAddress)) || listen(m_metricsSock, SOMAXCONN)) {
        perror("metrics bind/listen"); unInitNet(); return false;
    }
    // eventfd 用于业务线程安全唤醒阻塞中的 epoll_wait，
    // timerfd 让空闲连接检查也由 Reactor 统一处理。
    m_epollFd = epoll_create1(EPOLL_CLOEXEC);
    m_wakeupFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    m_timerFd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (m_epollFd < 0 || m_wakeupFd < 0 || m_timerFd < 0) {
        perror("epoll/eventfd/timerfd"); unInitNet(); return false;
    }

    itimerspec timerSpec{};
    timerSpec.it_value.tv_sec = kIdleCheckSeconds;
    timerSpec.it_interval.tv_sec = kIdleCheckSeconds;
    if (timerfd_settime(m_timerFd, 0, &timerSpec, nullptr) < 0) {
        perror("timerfd_settime"); unInitNet(); return false;
    }
    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = m_sock;
    if (epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_sock, &event) ||
        (event.data.fd = m_metricsSock, epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_metricsSock, &event)) ||
        (event.data.fd = m_wakeupFd, epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_wakeupFd, &event)) ||
        (event.data.fd = m_timerFd, epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_timerFd, &event))) {
        perror("epoll_ctl"); unInitNet(); return false;
    }
    m_isRunning = true;
    m_acceptPaused = false;
    m_lastAcceptLimitLog = {};
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        m_stopping = false;
        m_pendingJobCount = 0;
        ServerMetrics::instance().pendingJobs = 0;
        m_jobs.clear();
        m_waitingJobs.clear();
        m_activeJobConnections.clear();
    }
    // 启动业务线程池后再启动 I/O 线程，避免早到的数据没有消费者。
    for (size_t i = 0; i < m_workerCount; ++i) m_workers.emplace_back(&EpollTcpServer::workerRun, this);
    m_reactorThread = std::thread(&EpollTcpServer::run, this);
    Logger::info("reactor_started", {
        {"port", std::to_string(DEF_TCP_PORT)},
        {"metrics_port", std::to_string(m_metricsPort)},
        {"workers", std::to_string(m_workerCount)},
        {"max_pending_jobs", std::to_string(m_maxPendingJobs)}});
    return true;
}

bool EpollTcpServer::sendData(char* data, int len, uintptr_t to) {
    if (!data || len <= 0 || len > DEF_MAX_PACKET_SIZE || !m_isRunning) return false;
    // 网络帧格式固定为“4 字节网络序长度 + 协议体”。
    uint32_t wireLength = htonl(static_cast<uint32_t>(len));
    std::vector<char> bytes(sizeof(wireLength) + static_cast<size_t>(len));
    std::memcpy(bytes.data(), &wireLength, sizeof(wireLength));
    std::memcpy(bytes.data() + sizeof(wireLength), data, static_cast<size_t>(len));
    // 业务线程只入队，不直接操作 socket；I/O 线程是 socket 的唯一所有者。
    { std::lock_guard<std::mutex> lock(m_sendMutex); m_sendRequests.push_back({to, std::move(bytes)}); }
    uint64_t one = 1;
    if (write(m_wakeupFd, &one, sizeof(one)) < 0 && errno != EAGAIN) return false;
    return true;
}

void EpollTcpServer::run() {
    epoll_event events[kMaxEvents];
    while (m_isRunning) {
        int count = epoll_wait(m_epollFd, events, kMaxEvents, -1);
        if (count < 0) { if (errno == EINTR) continue; perror("epoll_wait"); break; }
        for (int i = 0; i < count; ++i) {
            int fd = events[i].data.fd;
            uint32_t flags = events[i].events;
            // 清空 eventfd 计数并批量接收业务线程提交的发送任务。
            if (fd == m_wakeupFd) { uint64_t ignored; while (read(m_wakeupFd, &ignored, sizeof(ignored)) > 0) {} drainSendRequests(); continue; }
            // timerfd 到期后检查所有连接，读出计数用于清除可读状态。
            if (fd == m_timerFd) { uint64_t ignored; while (read(m_timerFd, &ignored, sizeof(ignored)) > 0) {} checkIdleConnections(); continue; }
            if (fd == m_sock) { acceptReady(); continue; }
            if (fd == m_metricsSock) { acceptMetricsReady(); continue; }
            if (m_metricsConnections.find(fd) != m_metricsConnections.end()) {
                if (flags & (EPOLLERR | EPOLLHUP)) {
                    closeMetricsConnection(fd);
                    continue;
                }
                if (flags & (EPOLLIN | EPOLLRDHUP)) readMetricsReady(fd);
                if (m_metricsConnections.find(fd) != m_metricsConnections.end() &&
                    flags & EPOLLOUT) {
                    writeMetricsReady(fd);
                }
                continue;
            }
            // EPOLLERR表示连接已经出错，未处理任务不再可靠，直接关闭。
            if (flags & EPOLLERR) { closeConnection(fd); continue; }
            // 对端关闭写方向时，EPOLLIN和EPOLLRDHUP可能同时出现。
            // 必须先读尽内核缓冲区，不能先关闭而丢失最后一批完整协议帧。
            if (flags & (EPOLLIN | EPOLLHUP | EPOLLRDHUP)) readReady(fd);
            if (m_connections.find(fd) != m_connections.end() &&
                flags & (EPOLLHUP | EPOLLRDHUP)) {
                closeConnection(fd, true, false);
            }
            if (m_connections.find(fd) != m_connections.end() && flags & EPOLLOUT) writeReady(fd);
        }
    }
}

void EpollTcpServer::acceptMetricsReady() {
    for (;;) {
        int fd = accept4(m_metricsSock, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (errno == EMFILE || errno == ENFILE) {
                pauseAcceptingForFdLimit(errno);
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("metrics accept4");
            }
            return;
        }
        epoll_event event{};
        event.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
        event.data.fd = fd;
        if (epoll_ctl(m_epollFd, EPOLL_CTL_ADD, fd, &event) == 0) {
            m_metricsConnections.emplace(fd, MetricsConnection{fd, {}, {}, 0});
        } else {
            close(fd);
        }
    }
}

void EpollTcpServer::readMetricsReady(int fd) {
    auto it = m_metricsConnections.find(fd);
    if (it == m_metricsConnections.end()) return;
    char buffer[2048];
    bool peerClosed = false;
    for (;;) {
        const ssize_t received = recv(fd, buffer, sizeof(buffer), 0);
        if (received > 0) {
            it->second.input.append(buffer, static_cast<size_t>(received));
            if (it->second.input.size() > kMaxMetricsRequestSize) {
                closeMetricsConnection(fd);
                return;
            }
        } else if (received == 0) {
            peerClosed = true;
            break;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        } else {
            closeMetricsConnection(fd);
            return;
        }
    }

    it = m_metricsConnections.find(fd);
    if (it == m_metricsConnections.end()) return;
    if (it->second.output.empty() &&
        it->second.input.find("\r\n\r\n") != std::string::npos) {
        const bool found = it->second.input.rfind("GET /metrics ", 0) == 0;
        prepareMetricsResponse(it->second, found);
        epoll_event event{};
        event.events = EPOLLOUT | EPOLLRDHUP | EPOLLET;
        event.data.fd = fd;
        epoll_ctl(m_epollFd, EPOLL_CTL_MOD, fd, &event);
        // 立即尝试发送，避免依赖新的边沿触发可写事件。
        writeMetricsReady(fd);
        return;
    }
    if (peerClosed) closeMetricsConnection(fd);
}

void EpollTcpServer::writeMetricsReady(int fd) {
    auto it = m_metricsConnections.find(fd);
    if (it == m_metricsConnections.end() || it->second.output.empty()) return;
    MetricsConnection& connection = it->second;
    while (connection.outputOffset < connection.output.size()) {
        const ssize_t sent = send(
            fd,
            connection.output.data() + connection.outputOffset,
            connection.output.size() - connection.outputOffset,
            MSG_NOSIGNAL);
        if (sent > 0) {
            connection.outputOffset += static_cast<size_t>(sent);
        } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        } else {
            closeMetricsConnection(fd);
            return;
        }
    }
    // 每次抓取只处理一个请求，响应发送完成后主动关闭连接。
    closeMetricsConnection(fd);
}

std::string EpollTcpServer::buildMetricsBody() const {
    const ServerMetrics& metrics = ServerMetrics::instance();
    std::ostringstream output;
    output << "# HELP im_server_up IM服务端是否正在运行。\n"
        << "# TYPE im_server_up gauge\n"
        << "im_server_up 1\n"
        << "# TYPE im_server_active_connections gauge\n"
        << "im_server_active_connections " << metrics.activeConnections.load() << '\n'
        << "# TYPE im_server_accepted_connections_total counter\n"
        << "im_server_accepted_connections_total " << metrics.acceptedConnections.load() << '\n'
        << "# TYPE im_server_closed_connections_total counter\n"
        << "im_server_closed_connections_total " << metrics.closedConnections.load() << '\n'
        << "# TYPE im_server_received_packets_total counter\n"
        << "im_server_received_packets_total " << metrics.receivedPackets.load() << '\n'
        << "# TYPE im_server_sent_packets_total counter\n"
        << "im_server_sent_packets_total " << metrics.sentPackets.load() << '\n'
        << "# TYPE im_server_received_bytes_total counter\n"
        << "im_server_received_bytes_total " << metrics.receivedBytes.load() << '\n'
        << "# TYPE im_server_sent_bytes_total counter\n"
        << "im_server_sent_bytes_total " << metrics.sentBytes.load() << '\n'
        << "# TYPE im_server_pending_jobs gauge\n"
        << "im_server_pending_jobs " << metrics.pendingJobs.load() << '\n'
        << "# TYPE im_server_rejected_jobs_total counter\n"
        << "im_server_rejected_jobs_total " << metrics.rejectedJobs.load() << '\n'
        << "# HELP im_server_rate_limited_logins_total 被登录失败限流拒绝的请求总数。\n"
        << "# TYPE im_server_rate_limited_logins_total counter\n"
        << "im_server_rate_limited_logins_total " << metrics.rateLimitedLogins.load() << '\n'
        << "# TYPE im_server_mysql_pool_size gauge\n"
        << "im_server_mysql_pool_size " << metrics.databasePoolSize.load() << '\n'
        << "# TYPE im_server_mysql_busy_connections gauge\n"
        << "im_server_mysql_busy_connections " << metrics.databaseBusyConnections.load() << '\n'
        << "# TYPE im_server_mysql_operations_total counter\n"
        << "im_server_mysql_operations_total " << metrics.databaseOperations.load() << '\n'
        << "# TYPE im_server_mysql_failures_total counter\n"
        << "im_server_mysql_failures_total " << metrics.databaseFailures.load() << '\n'
        << "# TYPE im_server_mysql_acquire_timeouts_total counter\n"
        << "im_server_mysql_acquire_timeouts_total " << metrics.databaseAcquireTimeouts.load() << '\n';
    return output.str();
}

void EpollTcpServer::prepareMetricsResponse(
    MetricsConnection& connection,
    bool found) {
    const std::string body = found ? buildMetricsBody() : "not found\n";
    std::ostringstream response;
    response << (found ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 404 Not Found\r\n")
        << "Content-Type: "
        << (found ? "text/plain; version=0.0.4; charset=utf-8" : "text/plain; charset=utf-8")
        << "\r\nContent-Length: " << body.size()
        << "\r\nConnection: close\r\n\r\n"
        << body;
    connection.output = response.str();
}

void EpollTcpServer::closeMetricsConnection(int fd) {
    auto it = m_metricsConnections.find(fd);
    if (it == m_metricsConnections.end()) return;
    epoll_ctl(m_epollFd, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
    m_metricsConnections.erase(it);
    resumeAccepting();
}

void EpollTcpServer::pauseAcceptingForFdLimit(int errorNumber) {
    if (!m_acceptPaused) {
        // 监听socket保持打开，只从epoll临时移除，避免可读事件持续触发形成忙循环。
        epoll_ctl(m_epollFd, EPOLL_CTL_DEL, m_sock, nullptr);
        epoll_ctl(m_epollFd, EPOLL_CTL_DEL, m_metricsSock, nullptr);
        m_acceptPaused = true;
    }

    const auto now = std::chrono::steady_clock::now();
    if (m_lastAcceptLimitLog.time_since_epoch().count() == 0 ||
        now - m_lastAcceptLimitLog >= std::chrono::seconds(5)) {
        Logger::warning("accept_fd_limit_paused", {
            {"error", std::to_string(errorNumber)},
            {"active_connections", std::to_string(
                ServerMetrics::instance().activeConnections.load())}});
        m_lastAcceptLimitLog = now;
    }
}

void EpollTcpServer::resumeAccepting() {
    if (!m_acceptPaused || !m_isRunning) return;

    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = m_sock;
    if (epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_sock, &event) != 0) return;

    event.data.fd = m_metricsSock;
    if (epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_metricsSock, &event) != 0) {
        epoll_ctl(m_epollFd, EPOLL_CTL_DEL, m_sock, nullptr);
        return;
    }
    m_acceptPaused = false;
}

void EpollTcpServer::acceptReady() {
    for (;;) {
        // 边沿触发模式必须循环 accept，直到内核返回 EAGAIN。
        int fd = accept4(m_sock, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (errno == EMFILE || errno == ENFILE) {
                pauseAcceptingForFdLimit(errno);
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("accept4");
            }
            return;
        }
        epoll_event event{}; event.events = EPOLLIN | EPOLLRDHUP | EPOLLET; event.data.fd = fd;
        if (epoll_ctl(m_epollFd, EPOLL_CTL_ADD, fd, &event) == 0) {
            const uintptr_t connectionId = m_nextConnectionId++;
            m_connections.emplace(fd, Connection{fd, connectionId, {}, {}, 0, std::chrono::steady_clock::now()});
            m_connectionFds.emplace(connectionId, fd);
            ++ServerMetrics::instance().activeConnections;
            ++ServerMetrics::instance().acceptedConnections;
        } else {
            close(fd);
        }
    }
}

void EpollTcpServer::readReady(int fd) {
    auto it = m_connections.find(fd); if (it == m_connections.end()) return;
    char buffer[8192];
    bool peerClosed = false;
    // 边沿触发模式必须一次读尽 socket 接收缓冲区。
    for (;;) {
        ssize_t received = recv(fd, buffer, sizeof(buffer), 0);
        if (received > 0) {
            ServerMetrics::instance().receivedBytes += static_cast<uint64_t>(received);
            // 收到任意数据都说明客户端仍然活跃，包括业务包与心跳包。
            it->second.lastActive = std::chrono::steady_clock::now();
            it->second.input.insert(it->second.input.end(), buffer, buffer + received);
        }
        else if (received == 0) {
            // 先保留已经读到的字节，完成拆包后再关闭连接。
            peerClosed = true;
            break;
        }
        else { if (errno == EAGAIN || errno == EWOULDBLOCK) break; closeConnection(fd); return; }
    }
    auto& input = it->second.input;
    size_t consumed = 0;
    // 从缓存中依次拆出完整帧，半包继续留在 input 中等待下次可读事件。
    while (input.size() - consumed >= sizeof(uint32_t)) {
        uint32_t networkLength; std::memcpy(&networkLength, input.data() + consumed, sizeof(networkLength));
        uint32_t length = ntohl(networkLength);
        if (length == 0 || length > DEF_MAX_PACKET_SIZE) { closeConnection(fd); return; }
        size_t frameSize = sizeof(networkLength) + length;
        if (input.size() - consumed < frameSize) break;
        std::vector<char> packet(input.begin() + static_cast<ptrdiff_t>(consumed + sizeof(networkLength)), input.begin() + static_cast<ptrdiff_t>(consumed + frameSize));
        consumed += frameSize;
        ++ServerMetrics::instance().receivedPackets;
        if (!submitPacket(fd, std::move(packet))) {
            ++ServerMetrics::instance().rejectedJobs;
            Logger::warning("business_queue_full", {{"fd", std::to_string(fd)}});
            closeConnection(fd);
            return;
        }
    }
    if (consumed) input.erase(input.begin(), input.begin() + static_cast<ptrdiff_t>(consumed));
    if (input.size() > DEF_MAX_PACKET_SIZE + sizeof(uint32_t)) {
        closeConnection(fd);
        return;
    }
    if (peerClosed && m_connections.find(fd) != m_connections.end()) {
        // 完整尾包已经提交，断线任务排在这些业务包之后执行。
        closeConnection(fd, true, false);
    }
}

void EpollTcpServer::writeReady(int fd) {
    auto it = m_connections.find(fd); if (it == m_connections.end()) return;
    auto& connection = it->second;
    // 尽可能发送队首帧；遇到 EAGAIN 时保留偏移量，等待下一次 EPOLLOUT。
    while (!connection.output.empty()) {
        auto& bytes = connection.output.front();
        ssize_t sent = send(fd, bytes.data() + connection.outputOffset, bytes.size() - connection.outputOffset, MSG_NOSIGNAL);
        if (sent > 0) {
            ServerMetrics::instance().sentBytes += static_cast<uint64_t>(sent);
            connection.outputOffset += static_cast<size_t>(sent);
            if (connection.outputOffset == bytes.size()) {
                ++ServerMetrics::instance().sentPackets;
                connection.output.pop_front();
                connection.outputOffset = 0;
            }
        }
        else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        else { closeConnection(fd); return; }
    }
    if (m_connections.find(fd) != m_connections.end()) updateInterest(connection);
}

void EpollTcpServer::drainSendRequests() {
    std::deque<SendRequest> requests; { std::lock_guard<std::mutex> lock(m_sendMutex); requests.swap(m_sendRequests); }
    for (auto& request : requests) {
        auto fdIt = m_connectionFds.find(request.connectionId);
        if (fdIt == m_connectionFds.end()) continue;
        auto it = m_connections.find(fdIt->second); if (it == m_connections.end()) continue;
        size_t queued = it->second.outputOffset;
        for (const auto& item : it->second.output) queued += item.size();
        // 慢客户端长期不读会积压输出，超过上限时主动断开保护服务端。
        if (queued + request.bytes.size() > kMaxQueuedOutput) { closeConnection(fdIt->second); continue; }
        it->second.output.push_back(std::move(request.bytes));
        updateInterest(it->second);
    }
}

void EpollTcpServer::checkIdleConnections() {
    // 资源已经由其他路径释放时，定时重试可避免监听socket永久停留在暂停状态。
    resumeAccepting();
    const auto now = std::chrono::steady_clock::now();
    std::vector<int> expiredConnections;

    // 遍历期间先记录文件描述符，避免 closeConnection 使迭代器失效。
    for (const auto& entry : m_connections) {
        const auto idleSeconds = std::chrono::duration_cast<std::chrono::seconds>(
            now - entry.second.lastActive).count();
        if (idleSeconds >= kConnectionTimeoutSeconds) {
            expiredConnections.push_back(entry.first);
        }
    }

    for (int fd : expiredConnections) {
        Logger::warning("connection_idle_timeout", {{"fd", std::to_string(fd)}});
        closeConnection(fd);
    }

    if (++m_metricsTick % 3 == 0) {
        const ServerMetrics& metrics = ServerMetrics::instance();
        Logger::info("server_metrics", {
            {"active_connections", std::to_string(metrics.activeConnections.load())},
            {"accepted_connections", std::to_string(metrics.acceptedConnections.load())},
            {"closed_connections", std::to_string(metrics.closedConnections.load())},
            {"received_packets", std::to_string(metrics.receivedPackets.load())},
            {"sent_packets", std::to_string(metrics.sentPackets.load())},
            {"received_bytes", std::to_string(metrics.receivedBytes.load())},
            {"sent_bytes", std::to_string(metrics.sentBytes.load())},
            {"pending_jobs", std::to_string(metrics.pendingJobs.load())},
            {"rejected_jobs", std::to_string(metrics.rejectedJobs.load())},
            {"rate_limited_logins", std::to_string(metrics.rateLimitedLogins.load())},
            {"db_pool_size", std::to_string(metrics.databasePoolSize.load())},
            {"db_busy", std::to_string(metrics.databaseBusyConnections.load())},
            {"db_operations", std::to_string(metrics.databaseOperations.load())},
            {"db_failures", std::to_string(metrics.databaseFailures.load())},
            {"db_acquire_timeouts", std::to_string(metrics.databaseAcquireTimeouts.load())}});
    }
}

void EpollTcpServer::updateInterest(const Connection& connection) {
    epoll_event event{}; event.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
    if (!connection.output.empty()) event.events |= EPOLLOUT;
    event.data.fd = connection.fd;
    epoll_ctl(m_epollFd, EPOLL_CTL_MOD, connection.fd, &event);
}

bool EpollTcpServer::submitPacket(int fd, std::vector<char>&& packet) {
    auto connection = m_connections.find(fd);
    if (connection == m_connections.end()) return false;
    const uintptr_t connectionId = connection->second.id;
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        if (m_stopping || m_pendingJobCount >= m_maxPendingJobs) {
            return false;
        }
        ++m_pendingJobCount;
        ServerMetrics::instance().pendingJobs = m_pendingJobCount;
        // 首包可立即执行；同连接后续包等待当前任务完成，保证协议处理顺序。
        if (m_activeJobConnections.insert(connectionId).second) m_jobs.emplace_back(connectionId, std::move(packet));
        else m_waitingJobs[connectionId].push_back(std::move(packet));
    }
    m_jobCv.notify_one();
    return true;
}

void EpollTcpServer::workerRun() {
    for (;;) {
        std::pair<uintptr_t, std::vector<char>> job;
        { std::unique_lock<std::mutex> lock(m_jobMutex); m_jobCv.wait(lock, [this] { return m_stopping || !m_jobs.empty(); });
          if (m_jobs.empty()) { if (m_stopping) return; continue; } job = std::move(m_jobs.front()); m_jobs.pop_front(); }
        if (job.second.empty()) {
            // 空任务表示连接已经断开，清理工作在业务线程执行。
            m_pMediator->onDisconnected(job.first);
        } else {
            char* data = new char[job.second.size()];
            std::memcpy(data, job.second.data(), job.second.size());
            m_pMediator->transmitData(data, static_cast<int>(job.second.size()), job.first);
        }
        {
            std::lock_guard<std::mutex> lock(m_jobMutex);
            if (m_pendingJobCount > 0) --m_pendingJobCount;
            ServerMetrics::instance().pendingJobs = m_pendingJobCount;
            auto waiting = m_waitingJobs.find(job.first);
            // 当前包处理结束后，调度同连接的下一包或释放该连接的执行权。
            if (waiting != m_waitingJobs.end() && !waiting->second.empty()) {
                m_jobs.emplace_back(job.first, std::move(waiting->second.front()));
                waiting->second.pop_front();
                if (waiting->second.empty()) m_waitingJobs.erase(waiting);
                m_jobCv.notify_one();
            } else {
                m_activeJobConnections.erase(job.first);
            }
        }
    }
}

void EpollTcpServer::closeConnection(
    int fd,
    bool notifyMediator,
    bool discardPendingPackets) {
    auto it = m_connections.find(fd); if (it == m_connections.end()) return;
    const uintptr_t connectionId = it->second.id;
    m_connectionFds.erase(connectionId);
    epoll_ctl(m_epollFd, EPOLL_CTL_DEL, fd, nullptr); close(fd); m_connections.erase(it);
    // 关闭一个连接后通常已经腾出fd，可立即恢复因EMFILE/ENFILE暂停的accept。
    resumeAccepting();
    ++ServerMetrics::instance().closedConnections;
    if (ServerMetrics::instance().activeConnections.load() > 0) {
        --ServerMetrics::instance().activeConnections;
    }
    // 协议错误等异常断开可以丢弃等待任务；正常EOF必须保留已完整接收的尾包。
    // 两种情况都把断线清理排在该连接当前任务之后。
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        auto waiting = m_waitingJobs.find(connectionId);
        if (discardPendingPackets && waiting != m_waitingJobs.end()) {
            const size_t discarded = waiting->second.size();
            m_pendingJobCount = discarded > m_pendingJobCount
                    ? 0 : m_pendingJobCount - discarded;
            ServerMetrics::instance().pendingJobs = m_pendingJobCount;
            m_waitingJobs.erase(waiting);
        }
        if (notifyMediator && !m_stopping) {
            ++m_pendingJobCount;
            ServerMetrics::instance().pendingJobs = m_pendingJobCount;
            if (m_activeJobConnections.insert(connectionId).second) {
                m_jobs.emplace_back(connectionId, std::vector<char>{});
            } else {
                m_waitingJobs[connectionId].emplace_back();
            }
        }
    }
    if (notifyMediator) m_jobCv.notify_one();
}

void EpollTcpServer::unInitNet() {
    bool wasRunning = m_isRunning.exchange(false);
    // 停止接收新任务，并丢弃未开始的任务以缩短关闭时间。
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        m_stopping = true;
        m_jobs.clear();
        m_waitingJobs.clear();
    }
    m_jobCv.notify_all();
    if (m_wakeupFd >= 0) {
        uint64_t one = 1;
        [[maybe_unused]] ssize_t wakeResult = write(m_wakeupFd, &one, sizeof(one));
    }
    if (m_reactorThread.joinable()) m_reactorThread.join();
    for (auto& worker : m_workers) if (worker.joinable()) worker.join();
    m_workers.clear();
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        m_pendingJobCount = 0;
        ServerMetrics::instance().pendingJobs = 0;
        m_activeJobConnections.clear();
    }
    if (m_epollFd >= 0) {
        for (auto& entry : m_connections) close(entry.first);
        for (auto& entry : m_metricsConnections) close(entry.first);
        m_connections.clear();
        m_connectionFds.clear();
        m_metricsConnections.clear();
        close(m_epollFd);
        m_epollFd = -1;
    }
    if (m_wakeupFd >= 0) { close(m_wakeupFd); m_wakeupFd = -1; }
    if (m_timerFd >= 0) { close(m_timerFd); m_timerFd = -1; }
    if (m_metricsSock >= 0) { close(m_metricsSock); m_metricsSock = -1; }
    if (m_sock != INVALID_SOCKET) { close(m_sock); m_sock = INVALID_SOCKET; }
    ServerMetrics::instance().activeConnections = 0;
    if (wasRunning) Logger::info("reactor_stopped");
}
