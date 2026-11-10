#ifndef _RTCLOG_H_
#define _RTCLOG_H_

#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>

#ifdef __cplusplus
#include <string>
#include "easylogging++.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Log levels matching the project style
enum RtcLogLevel {
    RTC_FATAL = 0,
    RTC_ERROR = 1,
    RTC_WARN  = 2,
    RTC_INFO  = 3,
    RTC_DEBUG = 4,
    RTC_TRACE = 5
};

// Check if a specific log level is enabled
// 无锁：读一个原子位图（级别配置变化时刷新），可放在热路径上
bool rtclog_is_enabled(enum RtcLogLevel lvl);

// Main logging macro consistent with LLOG style
// Optimized to check level before formatting
#define RTCLOG(lvl, fmt, ...)                                          \
    do {                                                               \
        if (rtclog_is_enabled(lvl)) {                                  \
            rtclog_fmt(__FILE__, __LINE__, __FUNCTION__, lvl, fmt, ##__VA_ARGS__); \
        }                                                              \
    } while (0)

// Format function declaration
void rtclog_fmt(const char *filename, int fileline, const char* funcname, enum RtcLogLevel lvl,
              const char *fmt, ...) 
#ifdef __GNUC__
__attribute__((format(printf, 5, 6)))
#endif
;

// Initialization and configuration
void rtclog_init(const char *name);
void rtclog_set_level(enum RtcLogLevel lvl);
void rtclog_set_thread_name(const char *name);

// 异步写盘（默认开启）：调用线程只做格式化和一次无锁入队，由后台线程写文件；
// FATAL 级别仍同步写出（先排空队列）。环境变量 RTC_LOG_SYNC=1 或 rtclog_set_async(false) 改为同步写。
// 积压超过上限时丢弃新日志，并在恢复后输出丢弃条数。
// 注意：日志里的时间是写盘线程写出时的时间（通常比事件晚微秒级，积压时更多）；
// 进程崩溃（SIGSEGV、未捕获异常、assert）时队列里尚未写出的日志会丢失，调试崩溃时请用同步模式。
// fork 之后的子进程自动改为同步写。
void rtclog_set_async(bool on);
// 把队列中的日志同步写出（进程退出时会自动调用）
void rtclog_flush(void);

// Assertion Helpers (Ported from el_logger style)
#define RTC_ASSERT(expr)                                                       \
    do {                                                                       \
        if (!(expr)) {                                                         \
            RTCLOG(RTC_FATAL, "Assertion failed: %s", #expr);                  \
        }                                                                      \
    } while (0)

#define RTC_CHECK_AND_RET(expr, ret)                                           \
    do {                                                                       \
        if (!(expr)) {                                                         \
            return ret;                                                        \
        }                                                                      \
    } while (0)

#define RTC_CHECK_AND_RET_LOG(expr, ret, fmt, ...)                             \
    do {                                                                       \
        if (!(expr)) {                                                         \
            RTCLOG(RTC_ERROR, fmt, ##__VA_ARGS__);                             \
            return ret;                                                        \
        }                                                                      \
    } while (0)

#ifdef __cplusplus
} // extern "C"

#include <atomic>
#include <chrono>
#include <cstdint>

// 限频日志：同一调用点每 interval_ms 毫秒最多输出一条，并附带期间被丢弃的条数。
// 用于每包/每次投递等高频路径上的错误日志，fmt 必须是字符串字面量。
//   RTCLOG_EVERY_MS(RTC_WARN, 1000, "recvmmsg error: %s", strerror(err));
namespace rtclog_detail {
class RateLimiter {
public:
    bool allow(int64_t intervalMs, uint32_t* suppressed) {
        const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        int64_t last = last_.load(std::memory_order_relaxed);
        if ((last != 0 && now - last < intervalMs) ||
            !last_.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        *suppressed = dropped_.exchange(0, std::memory_order_relaxed);
        return true;
    }

private:
    std::atomic<int64_t> last_{0};
    std::atomic<uint32_t> dropped_{0};
};
} // namespace rtclog_detail

#define RTCLOG_EVERY_MS(lvl, interval_ms, fmt, ...)                                        \
    do {                                                                                   \
        if (rtclog_is_enabled(lvl)) {                                                      \
            static rtclog_detail::RateLimiter _rtclog_rl;                                  \
            uint32_t _rtclog_sup = 0;                                                      \
            if (_rtclog_rl.allow((interval_ms), &_rtclog_sup)) {                           \
                if (_rtclog_sup) {                                                         \
                    rtclog_fmt(__FILE__, __LINE__, __FUNCTION__, lvl,                      \
                               fmt " [限频: 上一周期另有 %u 条被丢弃]", ##__VA_ARGS__,         \
                               _rtclog_sup);                                               \
                } else {                                                                   \
                    rtclog_fmt(__FILE__, __LINE__, __FUNCTION__, lvl, fmt, ##__VA_ARGS__); \
                }                                                                          \
            }                                                                              \
        }                                                                                  \
    } while (0)

// C++ specific configuration helpers
void rtclog_configure(const std::string &filename_base = "", bool console = false,
                      const std::size_t max_log_file_size = 524288000, // 500 MB
                      const std::size_t max_log_files = 10);

#endif

#endif // _RTCLOG_H_
