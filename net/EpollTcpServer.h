#pragma once

#include "INet.h"
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Linux 平台的 Reactor 网络层：I/O 线程独占 socket 和 epoll，
// 已完整拆包的协议数据交由数量受限的业务线程池处理。
class EpollTcpServer final : public INet {
public:
    explicit EpollTcpServer(INetMediator* mediator);
    ~EpollTcpServer() override;
    bool initNet() override;
    bool sendData(char* data, int len, uintptr_t to) override;
    // 收包由 epoll 事件循环驱动，不再为每个客户端创建接收线程。
    void recvData() override {}
    void unInitNet() override;

private:
    struct Connection {
        int fd;                               // 客户端文件描述符。
        uintptr_t id;                         // 单调递增且不复用的连接编号。
        std::vector<char> input;              // 尚未拼成完整协议帧的输入缓存。
        std::deque<std::vector<char>> output; // 等待发送的完整网络帧队列。
        size_t outputOffset = 0;              // 队首帧已成功发送的字节数。
        std::chrono::steady_clock::time_point lastActive; // 最近一次收到数据的时间。
    };
    struct SendRequest { uintptr_t connectionId; std::vector<char> bytes; };
    struct MetricsConnection {
        int fd;                  // Prometheus抓取连接的文件描述符。
        std::string input;       // 尚未形成完整HTTP请求头的输入。
        std::string output;      // 等待非阻塞发送的HTTP响应。
        size_t outputOffset = 0; // 已发送的响应字节数。
    };

    void run();
    void acceptReady();
    void acceptMetricsReady();
    void pauseAcceptingForFdLimit(int errorNumber);
    void resumeAccepting();
    void readReady(int fd);
    void readMetricsReady(int fd);
    void writeReady(int fd);
    void writeMetricsReady(int fd);
    void closeConnection(
        int fd,
        bool notifyMediator = true,
        bool discardPendingPackets = true);
    void updateInterest(const Connection& connection);
    void drainSendRequests();
    void checkIdleConnections();
    std::string buildMetricsBody() const;
    void prepareMetricsResponse(MetricsConnection& connection, bool found);
    void closeMetricsConnection(int fd);
    bool submitPacket(int fd, std::vector<char>&& packet);
    void workerRun();
    bool setNonBlocking(int fd) const;

    int m_epollFd = -1;                       // epoll 实例。
    int m_wakeupFd = -1;                      // 用于唤醒 epoll 的 eventfd。
    int m_timerFd = -1;                       // 定时检查空闲连接的 timerfd。
    int m_metricsSock = -1;                   // Prometheus HTTP监听socket。
    std::thread m_reactorThread;              // 唯一 I/O 事件循环线程。
    std::vector<std::thread> m_workers;       // 业务处理线程池。
    std::unordered_map<int, Connection> m_connections; // fd 到连接状态，仅 I/O 线程访问。
    std::unordered_map<uintptr_t, int> m_connectionFds; // 唯一连接编号到当前 fd。
    std::unordered_map<int, MetricsConnection> m_metricsConnections; // HTTP监控连接。
    uintptr_t m_nextConnectionId = 1;          // 0 保留为无效连接编号。
    std::mutex m_sendMutex;                   // 保护跨线程发送请求队列。
    std::deque<SendRequest> m_sendRequests;   // 业务线程提交给 I/O 线程的发送任务。
    std::mutex m_jobMutex;                    // 保护业务任务队列与连接顺序状态。
    std::condition_variable m_jobCv;          // 业务线程等待任务的条件变量。
    std::deque<std::pair<uintptr_t, std::vector<char>>> m_jobs; // 可立即执行的任务。
    // TCP 是有序字节流：同一客户端同一时刻只处理一个包；不同客户端可并行处理。
    std::unordered_map<uintptr_t, std::deque<std::vector<char>>> m_waitingJobs;
    std::unordered_set<uintptr_t> m_activeJobConnections;
    std::size_t m_pendingJobCount = 0;         // 已接收但尚未处理完成的业务包数量。
    std::size_t m_workerCount = 4;             // 可通过环境变量调整的业务线程数。
    std::size_t m_maxPendingJobs = 10000;      // 有界队列容量，防止突发流量耗尽内存。
    unsigned int m_metricsTick = 0;            // 每三个10秒定时周期输出一次指标。
    uint16_t m_metricsPort = 9108;              // 可通过环境变量IM_METRICS_PORT调整。
    bool m_acceptPaused = false;               // 文件描述符耗尽时临时移除两个监听事件。
    std::chrono::steady_clock::time_point m_lastAcceptLimitLog; // 限制资源耗尽日志频率。
    std::atomic_bool m_stopping{false};       // 通知业务线程退出。
};
