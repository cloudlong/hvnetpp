#include "hvnetpp/EventLoop.h"
#include "rtclog.h"
#include <chrono>
#include <iostream>

using namespace hvnetpp;

int main() {
    // 初始化日志
    rtclog_init("TimerDemo");
    rtclog_configure("logs/demo.log", true);
    rtclog_set_level(RTC_DEBUG);

    EventLoop loop;

    // 1. runAfter: 延时执行一次
    // 延时 2500 毫秒执行
    loop.runAfter(std::chrono::milliseconds(2500), []() {
        RTCLOG(RTC_INFO, "runAfter 2.5s: This runs once.");
    });

    // 2. runEvery: 周期性执行
    // 每 1 秒执行一次
    TimerId everyId = loop.runEvery(std::chrono::seconds(1), []() {
        RTCLOG(RTC_INFO, "runEvery 1s: This runs every second.");
    });

    // 3. runAt: 在指定时间点执行 (很少直接使用，通常用 runAfter)
    // 这里演示如何取消定时器
    // 5.5 秒后取消上面的 periodic timer
    loop.runAfter(std::chrono::milliseconds(5500), [&loop, everyId]() {
        RTCLOG(RTC_INFO, "Cancelling the periodic timer...");
        loop.cancel(everyId);
    });

    // 10 秒后退出循环
    loop.runAfter(std::chrono::seconds(10), [&loop]() {
        RTCLOG(RTC_INFO, "Stopping loop...");
        loop.quit();
    });

    RTCLOG(RTC_INFO, "Starting EventLoop...");
    loop.loop();
    RTCLOG(RTC_INFO, "EventLoop stopped.");

    return 0;
}
