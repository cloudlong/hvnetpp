#include "hvnetpp/EventLoop.h"
#include "rtclog.h"
#include <iostream>

using namespace hvnetpp;

int main() {
    // 初始化日志
    rtclog_init("TimerDemo");
    rtclog_configure("logs/demo.log", true);
    rtclog_set_level(RTC_DEBUG);

    EventLoop loop;

    // 1. runAfter: 延时执行一次
    // 延时 2.5 秒执行
    loop.runAfter(2.5, []() {
        RTCLOG(RTC_INFO, "runAfter 2.5s: This runs once.");
    });

    // 2. runEvery: 周期性执行
    // 每 1.0 秒执行一次
    TimerId everyId = loop.runEvery(1.0, []() {
        RTCLOG(RTC_INFO, "runEvery 1s: This runs every second.");
    });

    // 3. runAt: 在指定时间点执行 (很少直接使用，通常用 runAfter)
    // 这里演示如何取消定时器
    // 5.5 秒后取消上面的 periodic timer
    loop.runAfter(5.5, [&loop, everyId]() {
        RTCLOG(RTC_INFO, "Cancelling the periodic timer...");
        loop.cancel(everyId);
    });

    // 10 秒后退出循环
    loop.runAfter(10.0, [&loop]() {
        RTCLOG(RTC_INFO, "Stopping loop...");
        loop.quit();
    });

    RTCLOG(RTC_INFO, "Starting EventLoop...");
    loop.loop();
    RTCLOG(RTC_INFO, "EventLoop stopped.");

    return 0;
}
