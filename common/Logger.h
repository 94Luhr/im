#pragma once

#include <map>
#include <string>

// 线程安全的 JSON 行日志，便于终端查看以及后续接入日志采集系统。
class Logger
{
public:
    static void info(const std::string& event,
                     const std::map<std::string, std::string>& fields = {});
    static void warning(const std::string& event,
                        const std::map<std::string, std::string>& fields = {});
    static void error(const std::string& event,
                      const std::map<std::string, std::string>& fields = {});

private:
    static void write(const char* level,
                      const std::string& event,
                      const std::map<std::string, std::string>& fields);
};
