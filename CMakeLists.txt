cmake_minimum_required(VERSION 3.10)

project(hvnetpp CXX)

if(NOT UNIX)
    message(FATAL_ERROR "hvnetpp is Linux-only (requires epoll/timerfd/eventfd)")
endif()

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)
find_package(Backtrace)

# 显式源文件列表（不用 GLOB，避免新增/删除文件时构建悄悄漏编）
set(HVNETPP_SOURCES
    src/Acceptor.cpp
    src/Buffer.cpp
    src/Channel.cpp
    src/CircularBuffer.cpp
    src/Connector.cpp
    src/EventLoop.cpp
    src/EventLoopThread.cpp
    src/EventLoopThreadPool.cpp
    src/InetAddress.cpp
    src/Poller.cpp
    src/SocketChannel.cpp
    src/SocketsOps.cpp
    src/TcpClient.cpp
    src/TcpServer.cpp
    src/Timer.cpp
    src/TimerQueue.cpp
    src/UdpClient.cpp
    src/UdpServer.cpp
    src/UdpSocket.cpp
    src/Unpack.cpp
    src/thirdparty/rtclog/easylogging++.cc
    src/thirdparty/rtclog/rtclog.cpp
)

add_library(hvnetpp STATIC ${HVNETPP_SOURCES})

target_include_directories(hvnetpp PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/thirdparty/rtclog>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(hvnetpp PUBLIC Threads::Threads)

if(Backtrace_FOUND)
    target_link_libraries(hvnetpp PRIVATE ${Backtrace_LIBRARIES})
    target_include_directories(hvnetpp PRIVATE ${Backtrace_INCLUDE_DIRS})
    target_compile_definitions(hvnetpp PRIVATE HAVE_BACKTRACE)
endif()

option(HVNETPP_BUILD_EXAMPLES "Build hvnetpp examples" ON)
if(HVNETPP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
