#include "rtclog.h"

#ifdef _WIN32
#include <windows.h>
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#endif
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <atomic>
#include <iomanip>
#include <vector>
#include <string>
#include <chrono>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <pthread.h>
#include <stdarg.h>
#include <thread>

#undef RTC_DEFAULT_LOG_CATEGORY
#define RTC_DEFAULT_LOG_CATEGORY "RTCLOG"

#define RTCLOG_BASE_FORMAT "%datetime{%Y-%M-%d %H:%m:%s.%g} %level %logger %fbase:%line %msg"

using namespace std;

// Helper functions
static std::vector<std::string>
vid_string_split(std::string str, std::string flag)
{
    std::vector<std::string> arr;
    if (flag.empty()) {
        arr.push_back(str);
        return arr;
    }
    size_t pos;
    std::string s = str;
    while ((pos = s.find(flag)) != std::string::npos) {
        if (pos != 0) {
            arr.push_back(s.substr(0, pos));
        }
        s = s.substr(pos + flag.length());
    }
    if (!s.empty()) {
        arr.push_back(s);
    }
    return arr;
}

extern "C"
{
#include <unistd.h>
}

std::string
rtclog_get_default_log_path(const char *default_filename)
{
    std::string log_path;
    char *buffer;
    if ((buffer = getcwd(NULL, 0)) != NULL) {
        log_path.append(buffer);
        log_path.append("/");
        free(buffer);
    }
    log_path.append("logs/");
    log_path.append(default_filename);
    return log_path;
}

static void rtclog_set_common_prefix() {}

static const char *
get_default_categories(int level)
{
    const char *categories = "";
    switch (level) {
        case 0:
        case 1:
            categories = "*:INFO,rtclog:INFO,global:INFO";
            break;
        case 2:
            categories = "*:DEBUG";
            break;
        case 3:
            categories = "*:TRACE";
            break;
        case 4:
            categories = "*:TRACE";
            break;
        default:
            break;
    }
    return categories;
}

// Forward declaration
void rtclog_set_log(const char *log);
static void rtclog_refresh_level_mask();

void
rtclog_configure(const std::string &filename_base, bool console, const std::size_t max_log_file_size,
               const std::size_t max_log_files)
{
    el::Configurations c;
    std::string _filename_base;

    if (filename_base.empty()) {
        _filename_base = rtclog_get_default_log_path("rtclog.log");
    } else if (filename_base.find("/") == std::string::npos) {
        _filename_base = rtclog_get_default_log_path(filename_base.c_str());
    } else {
        _filename_base = filename_base;
    }

    c.setGlobally(el::ConfigurationType::Filename, _filename_base);
    c.setGlobally(el::ConfigurationType::ToFile, "true");
    const char *log_format = getenv("RTC_LOG_FORMAT");
    if (!log_format)
        log_format = RTCLOG_BASE_FORMAT;
    c.setGlobally(el::ConfigurationType::Format, log_format);
    c.setGlobally(el::ConfigurationType::ToStandardOutput, console ? "true" : "false");
    c.setGlobally(el::ConfigurationType::MaxLogFileSize, std::to_string(max_log_file_size));
    
    el::Loggers::setDefaultConfigurations(c, true);

    // Also explicitly configure the rtclog category logger if it exists
    el::Logger* rtclogLogger = el::Loggers::getLogger(RTC_DEFAULT_LOG_CATEGORY);
    if (rtclogLogger) {
        rtclogLogger->configure(c);
    }

    el::Loggers::addFlag(el::LoggingFlag::AutoSpacing);
    el::Loggers::addFlag(el::LoggingFlag::HierarchicalLogging);
    el::Loggers::addFlag(el::LoggingFlag::NewLineForContainer);
    el::Loggers::addFlag(el::LoggingFlag::CreateLoggerAutomatically);
    el::Loggers::addFlag(el::LoggingFlag::DisableApplicationAbortOnFatalLog);
    el::Loggers::addFlag(el::LoggingFlag::ColoredTerminalOutput);
    el::Loggers::addFlag(el::LoggingFlag::StrictLogFileSizeCheck);

    el::Helpers::installPreRollOutCallback([_filename_base, max_log_files](const char *name, size_t size) {
        char abs_filename[PATH_MAX];
        std::string abs_file_path, rname;
        if (realpath(name, abs_filename)) {
            abs_file_path = std::string(abs_filename);
            const size_t last_slash_idx = abs_file_path.find_last_of("/");
            if (std::string::npos != last_slash_idx) {
                abs_file_path.erase(last_slash_idx + 1, -1);
            }
        } else {
            return;
        }

        // BACK IT UP
        std::stringstream ss;
        auto time_now = chrono::system_clock::now();
        auto duration_in_ms = chrono::duration_cast<chrono::milliseconds>(time_now.time_since_epoch());

        rname = abs_file_path + "rtclog-backup-" + std::to_string(duration_in_ms.count()) + ".log";

        int ret = rename(name, rname.c_str());
        if (ret < 0) {
            return;
        }

        if (max_log_files != 0) {
            std::vector<std::string> found_files;
            const char *dir_name = abs_file_path.c_str();
            struct stat s;
            stat(dir_name, &s);
            if (!S_ISDIR(s.st_mode)) {
                return;
            }

            struct dirent *fname;
            DIR *dir;

            dir = opendir(dir_name);
            if (NULL == dir) {
                return;
            }
            while ((fname = readdir(dir)) != NULL) {
                if (strcmp(fname->d_name, ".") == 0 || strcmp(fname->d_name, "..") == 0) {
                    continue;
                }

                if (fname->d_type != DT_REG) {
                    continue;
                }

                if (strncmp(fname->d_name, "rtclog-backup-", sizeof("rtclog-backup-") - 1) != 0) {
                    continue;
                }

                found_files.push_back(fname->d_name);
            }

            closedir(dir);

            if (found_files.size() >= max_log_files) {
                std::sort(found_files.begin(), found_files.end(), [](const std::string &fa, const std::string &fb) {
                    struct stat sa, sb;
                    if (stat(fa.c_str(), &sa) == 0 && stat(fb.c_str(), &sb) == 0) {
                        return sa.st_mtime < sb.st_mtime;
                    } else {
                        return strcmp(fa.c_str(), fb.c_str()) < 0;
                    }
                });

                for (size_t i = 0; i <= found_files.size() - max_log_files; ++i) {
                    std::string fname = abs_file_path + found_files[i];
                    remove(fname.c_str());
                }
            }
        }
    });
    rtclog_set_common_prefix();
    const char *rtc_log = getenv("RTC_LOGS");
    if (!rtc_log) {
        rtc_log = get_default_categories(0);
    }
    rtclog_set_log(rtc_log);
}

void
rtclog_set_categories(const char *categories)
{
    std::string new_categories;
    if (*categories) {
        if (*categories == '+') {
            ++categories;
            new_categories = el::Loggers::getCategories();
            if (*categories) {
                if (!new_categories.empty())
                    new_categories += ",";
                new_categories += categories;
            }
        } else if (*categories == '-') {
            ++categories;
            new_categories = el::Loggers::getCategories();
            std::vector<std::string> single_categories;
            single_categories = vid_string_split(categories, ",");

            for (const std::string &s : single_categories) {
                size_t pos = new_categories.find(s);
                if (pos != std::string::npos)
                    new_categories = new_categories.erase(pos, s.size());
            }
        } else {
            new_categories = categories;
        }
    }
    el::Loggers::setCategories(new_categories.c_str(), true);
    rtclog_refresh_level_mask();
}

void
rtclog_set_log_level(int level)
{
    const char *categories = get_default_categories(level);
    rtclog_set_categories(categories);
}

void
rtclog_set_log(const char *log)
{
    long level;
    char *ptr = NULL;

    if (!*log) {
        rtclog_set_categories(log);
        return;
    }
    level = strtol(log, &ptr, 10);
    if (ptr && *ptr) {
        if (*ptr == ',') {
            std::string new_categories = std::string(get_default_categories(level)) + ptr;
            rtclog_set_categories(new_categories.c_str());
        } else {
            rtclog_set_categories(log);
        }
    } else if (level >= 0 && level <= 4) {
        rtclog_set_log_level(level);
    } else {
        // Fallback logging if rtclog not ready? Just printf for now or ignore
        printf("Invalid numerical log level: %s\n", log);
    }
}

// ----------------------------------------------------------------------
// Implementation of New Interfaces
// ----------------------------------------------------------------------

void rtclog_init(const char *name) {
    rtclog_configure(name ? std::string(name) : "", true);
}

void rtclog_set_level(enum RtcLogLevel lvl) {
    // Map internal logic level to categories logic
    // The previous logic used 0-4 levels for categories setup
    // 0,1: INFO+; 2: DEBUG; 3,4: TRACE
    
    int internal_level = 0;
    switch(lvl) {
        case RTC_FATAL: 
        case RTC_ERROR: 
        case RTC_WARN:  
        case RTC_INFO:  internal_level = 1; break;
        case RTC_DEBUG: internal_level = 2; break;
        case RTC_TRACE: internal_level = 4; break;
    }
    rtclog_set_log_level(internal_level);
}

void rtclog_set_thread_name(const char *name) {
    if (name) {
        el::Helpers::setThreadName(name);
    }
}

static el::Level rtc_level_to_el(enum RtcLogLevel lvl) {
    switch(lvl) {
        case RTC_FATAL: return el::Level::Fatal;
        case RTC_ERROR: return el::Level::Error;
        case RTC_WARN:  return el::Level::Warning;
        case RTC_INFO:  return el::Level::Info;
        case RTC_DEBUG: return el::Level::Debug;
        case RTC_TRACE: return el::Level::Trace;
        default: return el::Level::Info;
    }
}

// ----------------------------------------------------------------------
// 级别检查：原子位图。easylogging 的 VRegistry::allowed 每次都要拿全局锁，
// 这里只在级别配置变化时（rtclog_set_categories）调用它刷新位图，热路径只读一个原子变量。
// ----------------------------------------------------------------------
static std::atomic<uint32_t> g_level_mask(0);

static void rtclog_refresh_level_mask() {
    uint32_t mask = 0;
    for (int l = RTC_FATAL; l <= RTC_TRACE; ++l) {
        if (ELPP->vRegistry()->allowed(rtc_level_to_el(static_cast<RtcLogLevel>(l)), RTC_DEFAULT_LOG_CATEGORY)) {
            mask |= 1u << l;
        }
    }
    g_level_mask.store(mask, std::memory_order_release);
}

bool rtclog_is_enabled(enum RtcLogLevel lvl) {
    return ((g_level_mask.load(std::memory_order_relaxed) >> static_cast<unsigned>(lvl)) & 1u) != 0;
}

// ----------------------------------------------------------------------
// 异步写盘：调用线程格式化后一次 malloc + 无锁入队（Vyukov MPSC），后台线程负责写文件。
// ----------------------------------------------------------------------
namespace {

struct LogRecord {
    std::atomic<LogRecord*> next;
    el::Level level;
    const char* file;    // __FILE__ / __FUNCTION__ 均为静态字符串，可跨线程使用
    int line;
    const char* func;
    char msg[1];         // 变长
};

LogRecord* newRecord(el::Level level, const char* file, int line, const char* func, const char* msg, size_t len) {
    void* mem = std::malloc(offsetof(LogRecord, msg) + len + 1);
    if (!mem) {
        return nullptr;
    }
    LogRecord* r = static_cast<LogRecord*>(mem);
    new (&r->next) std::atomic<LogRecord*>(nullptr);
    r->level = level;
    r->file = file;
    r->line = line;
    r->func = func;
    std::memcpy(r->msg, msg, len);
    r->msg[len] = '\0';
    return r;
}

void writeRecord(el::Level level, const char* file, int line, const char* func, const char* msg) {
    el::base::Writer(level, el::Color::Default, file, line, func).construct(RTC_DEFAULT_LOG_CATEGORY) << msg;
}

class AsyncLogger {
public:
    static const int64_t kMaxPending = 1000000;   // 积压上限，超过丢弃新日志

    AsyncLogger() : head_(&stub_), tail_(&stub_) {
        stub_.next.store(nullptr, std::memory_order_relaxed);
    }

    // 返回 false 表示应退回同步写（后台线程已停止）
    bool post(LogRecord* r) {
        if (stopped_.load(std::memory_order_acquire)) {
            return false;
        }
        ensureStarted();
        if (pending_.fetch_add(1, std::memory_order_relaxed) >= kMaxPending) {
            pending_.fetch_sub(1, std::memory_order_relaxed);
            dropped_.fetch_add(1, std::memory_order_relaxed);
            std::free(r);
            return true;
        }
        push(r);
        if (!wake_.exchange(true, std::memory_order_acq_rel)) {
            std::lock_guard<std::mutex> lock(mutex_);
            cond_.notify_one();
        }
        return true;
    }

    // 把已入队的日志全部写出（可在任意线程调用，与后台线程互斥）
    void drain() {
        if (forked_.load(std::memory_order_acquire)) {
            return;   // fork 出的子进程：写盘线程不存在，锁的状态也不可信，放弃队列里的旧日志
        }
        std::lock_guard<std::mutex> lock(drainMutex_);
        while (LogRecord* r = pop()) {
            writeRecord(r->level, r->file, r->line, r->func, r->msg);
            std::free(r);
            pending_.fetch_sub(1, std::memory_order_relaxed);
        }
        const int64_t dropped = dropped_.exchange(0, std::memory_order_relaxed);
        if (dropped > 0) {
            char buf[128];
            snprintf(buf, sizeof buf, "rtclog: 异步日志积压超过 %lld 条，丢弃 %lld 条",
                     static_cast<long long>(kMaxPending), static_cast<long long>(dropped));
            writeRecord(el::Level::Warning, __FILE__, __LINE__, __FUNCTION__, buf);
        }
    }

    void shutdown() {
        if (forked_.load(std::memory_order_acquire)) {
            return;   // 子进程里没有写盘线程，不能 join
        }
        bool expected = false;
        if (!stopped_.compare_exchange_strong(expected, true)) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cond_.notify_one();
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        drain();
    }

private:
    static void atExit();
    static void atForkChild();

    void ensureStarted() {
        std::call_once(startOnce_, [this] {
            thread_ = std::thread(&AsyncLogger::run, this);
            std::atexit(&AsyncLogger::atExit);
            pthread_atfork(nullptr, nullptr, &AsyncLogger::atForkChild);
        });
    }

    void run() {
        pthread_setname_np(pthread_self(), "rtclog");
        while (!stopped_.load(std::memory_order_acquire)) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cond_.wait_for(lock, std::chrono::milliseconds(200), [this] {
                    return wake_.load(std::memory_order_acquire) || stopped_.load(std::memory_order_acquire);
                });
            }
            // 先清标志再取：此后入队的生产者会再次通知，不会丢唤醒
            wake_.exchange(false, std::memory_order_acq_rel);
            drain();
        }
    }

    void push(LogRecord* r) {
        r->next.store(nullptr, std::memory_order_relaxed);
        LogRecord* prev = head_.exchange(r, std::memory_order_acq_rel);
        prev->next.store(r, std::memory_order_release);
    }

    // 单消费者（由 drainMutex_ 保证）
    LogRecord* pop() {
        LogRecord* tail = tail_;
        LogRecord* next = tail->next.load(std::memory_order_acquire);
        if (tail == &stub_) {
            if (!next) return nullptr;
            tail_ = next;
            tail = next;
            next = next->next.load(std::memory_order_acquire);
        }
        if (next) {
            tail_ = next;
            return tail;
        }
        if (tail != head_.load(std::memory_order_acquire)) return nullptr;
        push(&stub_);
        next = tail->next.load(std::memory_order_acquire);
        if (next) {
            tail_ = next;
            return tail;
        }
        return nullptr;
    }

    std::atomic<LogRecord*> head_;
    char pad_[64];
    LogRecord* tail_;
    LogRecord stub_;
    std::atomic<int64_t> pending_{0};
    std::atomic<int64_t> dropped_{0};
    std::atomic<bool> wake_{false};
    std::atomic<bool> stopped_{false};
    std::atomic<bool> forked_{false};
    std::mutex mutex_;
    std::condition_variable cond_;
    std::mutex drainMutex_;
    std::once_flag startOnce_;
    std::thread thread_;
};

// 故意泄漏：避免静态析构顺序问题（进程退出时由 atexit 回调 shutdown）
AsyncLogger& asyncLogger() {
    static AsyncLogger* logger = new AsyncLogger;
    return *logger;
}

void AsyncLogger::atExit() {
    asyncLogger().shutdown();
}

std::atomic<bool> g_async(getenv("RTC_LOG_SYNC") == NULL || strcmp(getenv("RTC_LOG_SYNC"), "1") != 0);

// 子进程（如 daemon() 发生在首次打日志之后）：写盘线程没有被复制过来，改为同步写
void AsyncLogger::atForkChild() {
    AsyncLogger& logger = asyncLogger();
    logger.forked_.store(true, std::memory_order_release);
    logger.stopped_.store(true, std::memory_order_release);
    g_async.store(false, std::memory_order_relaxed);
}

void emit(el::Level level, const char* file, int line, const char* func, const char* msg, size_t len) {
    if (level != el::Level::Fatal && g_async.load(std::memory_order_relaxed)) {
        LogRecord* r = newRecord(level, file, line, func, msg, len);
        if (r && asyncLogger().post(r)) {
            return;
        }
        std::free(r);
    } else if (level == el::Level::Fatal) {
        // FATAL 之后进程通常会 abort：先把积压的日志写出，保证顺序和完整性
        asyncLogger().drain();
    }
    writeRecord(level, file, line, func, msg);
}

} // namespace

void rtclog_set_async(bool on) {
    if (!on) {
        asyncLogger().drain();
    }
    g_async.store(on, std::memory_order_relaxed);
}

void rtclog_flush(void) {
    asyncLogger().drain();
}

void rtclog_fmt(const char *filename, int fileline, const char* funcname, enum RtcLogLevel lvl, const char *fmt, ...) {
    if (!rtclog_is_enabled(lvl)) {
        return;
    }
    el::Level el_lvl = rtc_level_to_el(lvl);

    va_list args;
    va_start(args, fmt);

    // 小消息用栈缓冲格式化
    char stack_buf[1024];
    va_list args_copy;
    va_copy(args_copy, args);
    int len = vsnprintf(stack_buf, sizeof(stack_buf), fmt, args_copy);
    va_end(args_copy);

    if (len < 0) {
        va_end(args);
        return;
    }

    if (static_cast<size_t>(len) < sizeof(stack_buf)) {
        emit(el_lvl, filename, fileline, funcname, stack_buf, static_cast<size_t>(len));
    } else {
        std::string message(len, '\0');
        vsnprintf(&message[0], len + 1, fmt, args);
        emit(el_lvl, filename, fileline, funcname, message.data(), message.size());
    }

    va_end(args);
}
