#include "hvnetpp/Timer.h"

namespace hvnetpp {

std::atomic<int64_t> Timer::s_numCreated_(0);

void Timer::restart(Timestamp now) {
    if (repeat_) {
        expiration_ = now + std::chrono::milliseconds(static_cast<int64_t>(interval_ * 1000));
    } else {
        expiration_ = Timestamp(); // invalid
    }
}

} // namespace hvnetpp
