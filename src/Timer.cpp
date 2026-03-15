#include "hvnetpp/Timer.h"

namespace hvnetpp {

std::atomic<int64_t> Timer::s_numCreated_(0);

void Timer::restart(Timestamp now) {
    if (repeat_) {
        const auto interval = std::chrono::microseconds(static_cast<int64_t>(interval_ * 1000000));
        if (interval.count() > 0) {
            expiration_ += interval;
            if (expiration_ <= now) {
                const auto overdue = std::chrono::duration_cast<std::chrono::microseconds>(now - expiration_);
                const int64_t skipped = overdue.count() / interval.count() + 1;
                expiration_ += interval * skipped;
            }
        } else {
            expiration_ = now;
        }
    } else {
        expiration_ = Timestamp(); // invalid
    }
}

} // namespace hvnetpp
