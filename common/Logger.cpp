#include "Logger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

namespace {
std::mutex g_logMutex;

std::string escapeJson(const std::string& text)
{
    std::ostringstream output;
    for (unsigned char character : text) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20) {
                output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<int>(character) << std::dec;
            } else {
                output << static_cast<char>(character);
            }
        }
    }
    return output.str();
}

std::string utcTimestamp()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t value = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_r(&value, &utc);
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}
}

void Logger::info(const std::string& event,
                  const std::map<std::string, std::string>& fields)
{
    write("INFO", event, fields);
}

void Logger::warning(const std::string& event,
                     const std::map<std::string, std::string>& fields)
{
    write("WARN", event, fields);
}

void Logger::error(const std::string& event,
                   const std::map<std::string, std::string>& fields)
{
    write("ERROR", event, fields);
}

void Logger::write(const char* level,
                   const std::string& event,
                   const std::map<std::string, std::string>& fields)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    std::ostream& output = std::string(level) == "ERROR" ? std::cerr : std::cout;
    output << "{\"time\":\"" << utcTimestamp()
           << "\",\"level\":\"" << level
           << "\",\"event\":\"" << escapeJson(event) << '"';
    for (const auto& field : fields) {
        output << ",\"" << escapeJson(field.first) << "\":\""
               << escapeJson(field.second) << '"';
    }
    output << '}' << std::endl;
}
