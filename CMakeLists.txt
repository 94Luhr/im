cmake_minimum_required(VERSION 3.16)
project(im_server_linux LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "This CMake target is the Linux epoll server. Build it on Linux or WSL.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_path(MYSQL_INCLUDE_DIR mysql.h PATH_SUFFIXES mysql mariadb)
find_library(MYSQL_LIBRARY NAMES mysqlclient mariadb)
if(NOT MYSQL_INCLUDE_DIR OR NOT MYSQL_LIBRARY)
    message(FATAL_ERROR "MySQL/MariaDB development package not found. Install default-libmysqlclient-dev or libmariadb-dev.")
endif()

add_executable(im_server_linux
    LinuxMain.cpp CKernel.cpp MySQL/CMySql.cpp common/Logger.cpp
    mediator/TcpServerMediator.cpp net/EpollTcpServer.cpp)
target_include_directories(im_server_linux PRIVATE ${MYSQL_INCLUDE_DIR})
target_link_libraries(im_server_linux PRIVATE Threads::Threads OpenSSL::Crypto ${MYSQL_LIBRARY})
target_compile_options(im_server_linux PRIVATE -Wall -Wextra -Wpedantic)
