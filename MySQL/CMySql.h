#pragma once

#include <mysql.h>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <list>
#include <mutex>
#include <string>

// 线程安全的 MySQL 连接池。每次数据库操作独占一个连接，结束后自动归还。
class CMySql
{
public:
    CMySql();
    ~CMySql();

    bool ConnectMySql(const char* host,
                      const char* user,
                      const char* pass,
                      const char* db,
                      short port = 3306,
                      std::size_t poolSize = 4);
    void DisConnect();

    bool SelectMySql(const char* sql, int columnCount, std::list<std::string>& values);
    bool GetTables(const char* sql, std::list<std::string>& values);
    bool UpdateMySql(const char* sql);
    bool InsertMySql(const char* sql, uint64_t& insertId);
    bool UpdateMySqlTransaction(const char* firstSql, const char* secondSql);
    bool EscapeString(const char* source, std::string& escaped);

    std::size_t PoolSize() const;
    std::size_t BusyConnections() const;

private:
    class ConnectionLease
    {
    public:
        explicit ConnectionLease(CMySql& owner);
        ~ConnectionLease();
        MYSQL* get() const { return m_connection; }
        explicit operator bool() const { return m_connection != nullptr; }

        ConnectionLease(const ConnectionLease&) = delete;
        ConnectionLease& operator=(const ConnectionLease&) = delete;

    private:
        CMySql& m_owner;
        MYSQL* m_connection;
    };

    MYSQL* createConnection() const;
    MYSQL* acquireConnection();
    void releaseConnection(MYSQL* connection);

    mutable std::mutex m_poolMutex;
    std::condition_variable m_poolCv;
    std::deque<MYSQL*> m_idleConnections;
    std::size_t m_poolSize = 0;
    std::size_t m_connectionCount = 0;
    std::size_t m_busyConnections = 0;
    bool m_stopping = true;

    std::string m_host;
    std::string m_user;
    std::string m_password;
    std::string m_database;
    unsigned int m_port = 3306;
};
