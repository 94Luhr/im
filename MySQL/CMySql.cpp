#include "CMySql.h"
#include "../common/Logger.h"
#include "../common/ServerMetrics.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <utility>
#include <vector>

namespace {
constexpr std::size_t kMaxPoolSize = 64;
constexpr auto kAcquireTimeout = std::chrono::seconds(3);

class DatabaseOperation
{
public:
    DatabaseOperation() { ++ServerMetrics::instance().databaseOperations; }
    ~DatabaseOperation() {
        if (!m_success) ++ServerMetrics::instance().databaseFailures;
    }
    void succeed() { m_success = true; }
private:
    bool m_success = false;
};
}

CMySql::CMySql() = default;

CMySql::~CMySql()
{
    DisConnect();
}

CMySql::ConnectionLease::ConnectionLease(CMySql& owner)
    : m_owner(owner), m_connection(owner.acquireConnection())
{
}

CMySql::ConnectionLease::~ConnectionLease()
{
    if (m_connection != nullptr) {
        m_owner.releaseConnection(m_connection);
    }
}

MYSQL* CMySql::createConnection() const
{
    MYSQL* connection = mysql_init(nullptr);
    if (connection == nullptr) {
        return nullptr;
    }

    // 禁用已弃用的 MYSQL_OPT_RECONNECT，健康检查失败后由连接池显式重建连接。
    unsigned int connectTimeout = 3;
    mysql_options(connection, MYSQL_OPT_CONNECT_TIMEOUT, &connectTimeout);
    mysql_options(connection, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    if (mysql_real_connect(connection,
                           m_host.c_str(),
                           m_user.c_str(),
                           m_password.c_str(),
                           m_database.c_str(),
                           m_port,
                           nullptr,
                           0) == nullptr) {
        Logger::error("mysql_connect_failed", {{"error", mysql_error(connection)}});
        mysql_close(connection);
        return nullptr;
    }
    if (mysql_set_character_set(connection, "utf8mb4") != 0) {
        Logger::error("mysql_charset_failed", {{"error", mysql_error(connection)}});
        mysql_close(connection);
        return nullptr;
    }
    return connection;
}

bool CMySql::ConnectMySql(const char* host,
                          const char* user,
                          const char* pass,
                          const char* db,
                          short port,
                          std::size_t poolSize)
{
    if (host == nullptr || user == nullptr || pass == nullptr || db == nullptr) {
        return false;
    }

    DisConnect();
    m_host = host;
    m_user = user;
    m_password = pass;
    m_database = db;
    m_port = static_cast<unsigned short>(port);
    m_poolSize = std::max<std::size_t>(1, std::min(poolSize, kMaxPoolSize));

    std::vector<MYSQL*> created;
    created.reserve(m_poolSize);
    for (std::size_t index = 0; index < m_poolSize; ++index) {
        MYSQL* connection = createConnection();
        if (connection == nullptr) {
            for (MYSQL* item : created) {
                mysql_close(item);
            }
            return false;
        }
        created.push_back(connection);
    }

    {
        std::lock_guard<std::mutex> lock(m_poolMutex);
        m_stopping = false;
        m_connectionCount = created.size();
        for (MYSQL* connection : created) {
            m_idleConnections.push_back(connection);
        }
    }
    ServerMetrics::instance().databasePoolSize = m_poolSize;
    Logger::info("mysql_pool_started", {{"pool_size", std::to_string(m_poolSize)}});
    return true;
}

void CMySql::DisConnect()
{
    std::deque<MYSQL*> connections;
    {
        std::unique_lock<std::mutex> lock(m_poolMutex);
        m_stopping = true;
        m_poolCv.notify_all();
        // 服务端先停止业务线程再析构连接池；这里仍等待在途 SQL 安全结束。
        m_poolCv.wait(lock, [this] { return m_busyConnections == 0; });
        connections.swap(m_idleConnections);
        m_connectionCount = 0;
        m_poolSize = 0;
        ServerMetrics::instance().databasePoolSize = 0;
        ServerMetrics::instance().databaseBusyConnections = 0;
    }
    for (MYSQL* connection : connections) {
        mysql_close(connection);
    }
}

MYSQL* CMySql::acquireConnection()
{
    MYSQL* connection = nullptr;
    {
        std::unique_lock<std::mutex> lock(m_poolMutex);
        if (!m_poolCv.wait_for(lock, kAcquireTimeout, [this] {
                return m_stopping || !m_idleConnections.empty() ||
                       m_connectionCount < m_poolSize;
            })) {
            ++ServerMetrics::instance().databaseAcquireTimeouts;
            Logger::warning("mysql_acquire_timeout");
            return nullptr;
        }
        if (m_stopping) {
            return nullptr;
        }
        if (!m_idleConnections.empty()) {
            connection = m_idleConnections.front();
            m_idleConnections.pop_front();
        } else {
            // 健康检查淘汰坏连接后，按需补足池容量。
            ++m_connectionCount;
        }
        ++m_busyConnections;
        ServerMetrics::instance().databaseBusyConnections = m_busyConnections;
    }

    if (connection == nullptr) {
        connection = createConnection();
        if (connection == nullptr) {
            std::lock_guard<std::mutex> lock(m_poolMutex);
            --m_busyConnections;
            --m_connectionCount;
            ServerMetrics::instance().databaseBusyConnections = m_busyConnections;
            ServerMetrics::instance().databasePoolSize = m_connectionCount;
            m_poolCv.notify_all();
            return nullptr;
        }
    }

    // 连接由当前线程独占，可以在池锁之外执行健康检查。
    if (mysql_ping(connection) != 0) {
        mysql_close(connection);
        connection = createConnection();
        if (connection == nullptr) {
            std::lock_guard<std::mutex> lock(m_poolMutex);
            --m_busyConnections;
            --m_connectionCount;
            ServerMetrics::instance().databaseBusyConnections = m_busyConnections;
            ServerMetrics::instance().databasePoolSize = m_connectionCount;
            m_poolCv.notify_all();
        }
    }
    return connection;
}

void CMySql::releaseConnection(MYSQL* connection)
{
    bool shouldClose = false;
    {
        std::lock_guard<std::mutex> lock(m_poolMutex);
        if (m_busyConnections > 0) {
            --m_busyConnections;
        }
        if (m_stopping) {
            shouldClose = true;
            if (m_connectionCount > 0) {
                --m_connectionCount;
            }
        } else {
            m_idleConnections.push_back(connection);
        }
        ServerMetrics::instance().databaseBusyConnections = m_busyConnections;
        ServerMetrics::instance().databasePoolSize = m_connectionCount;
        m_poolCv.notify_all();
    }
    if (shouldClose) {
        mysql_close(connection);
    }
}

bool CMySql::SelectMySql(const char* sql,
                         int columnCount,
                         std::list<std::string>& values)
{
    DatabaseOperation operation;
    values.clear();
    if (sql == nullptr || columnCount <= 0) {
        return false;
    }
    ConnectionLease lease(*this);
    if (!lease || mysql_query(lease.get(), sql) != 0) {
        if (lease) {
            Logger::error("mysql_query_failed", {{"error", mysql_error(lease.get())}});
        }
        return false;
    }

    MYSQL_RES* result = mysql_store_result(lease.get());
    if (result == nullptr) {
        return false;
    }
    if (static_cast<unsigned int>(columnCount) > mysql_num_fields(result)) {
        mysql_free_result(result);
        return false;
    }

    MYSQL_ROW row = nullptr;
    while ((row = mysql_fetch_row(result)) != nullptr) {
        for (int column = 0; column < columnCount; ++column) {
            values.push_back(row[column] == nullptr ? "" : row[column]);
        }
    }
    mysql_free_result(result);
    operation.succeed();
    return true;
}

bool CMySql::GetTables(const char* sql, std::list<std::string>& values)
{
    return SelectMySql(sql, 1, values);
}

bool CMySql::UpdateMySql(const char* sql)
{
    DatabaseOperation operation;
    if (sql == nullptr) {
        return false;
    }
    ConnectionLease lease(*this);
    if (!lease || mysql_query(lease.get(), sql) != 0) {
        if (lease) {
            Logger::error("mysql_update_failed", {{"error", mysql_error(lease.get())}});
        }
        return false;
    }
    operation.succeed();
    return true;
}

bool CMySql::InsertMySql(const char* sql, uint64_t& insertId)
{
    DatabaseOperation operation;
    insertId = 0;
    if (sql == nullptr) {
        return false;
    }
    ConnectionLease lease(*this);
    if (!lease || mysql_query(lease.get(), sql) != 0) {
        if (lease) {
            Logger::error("mysql_insert_failed", {{"error", mysql_error(lease.get())}});
        }
        return false;
    }
    insertId = static_cast<uint64_t>(mysql_insert_id(lease.get()));
    if (insertId == 0) return false;
    operation.succeed();
    return true;
}

bool CMySql::UpdateMySqlTransaction(const char* firstSql, const char* secondSql)
{
    DatabaseOperation operation;
    if (firstSql == nullptr || secondSql == nullptr) {
        return false;
    }
    ConnectionLease lease(*this);
    if (!lease) {
        return false;
    }

    MYSQL* connection = lease.get();
    if (mysql_query(connection, "START TRANSACTION") != 0) {
        return false;
    }
    if (mysql_query(connection, firstSql) != 0 ||
        mysql_query(connection, secondSql) != 0 ||
        mysql_query(connection, "COMMIT") != 0) {
        Logger::error("mysql_transaction_failed", {{"error", mysql_error(connection)}});
        mysql_query(connection, "ROLLBACK");
        return false;
    }
    operation.succeed();
    return true;
}

bool CMySql::EscapeString(const char* source, std::string& escaped)
{
    DatabaseOperation operation;
    if (source == nullptr) {
        return false;
    }
    ConnectionLease lease(*this);
    if (!lease) {
        return false;
    }

    const std::size_t sourceLength = std::strlen(source);
    std::string buffer(sourceLength * 2 + 1, '\0');
    const unsigned long escapedLength = mysql_real_escape_string(
        lease.get(), &buffer[0], source, static_cast<unsigned long>(sourceLength));
    escaped.assign(buffer.data(), escapedLength);
    operation.succeed();
    return true;
}

std::size_t CMySql::PoolSize() const
{
    std::lock_guard<std::mutex> lock(m_poolMutex);
    return m_connectionCount;
}

std::size_t CMySql::BusyConnections() const
{
    std::lock_guard<std::mutex> lock(m_poolMutex);
    return m_busyConnections;
}
