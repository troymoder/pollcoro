module;

#if !defined(POLLCORO_IMPORT_STD) || POLLCORO_IMPORT_STD == 0
#include <cstddef>
#endif

export module pollcoro:yield;

#if defined(POLLCORO_IMPORT_STD) && POLLCORO_IMPORT_STD == 1
import std;
#endif

import :awaitable;
import :is_blocking;
import :waker;

export namespace pollcoro {
class yield_awaitable : public awaitable_always_blocks {
    std::size_t ready_{0};

  public:
    yield_awaitable() : yield_awaitable(1) {}

    explicit yield_awaitable(std::size_t ready) : ready_(ready) {}

    awaitable_state<> poll(const waker& w) {
        if (ready_ > 0) {
            ready_--;
        }
        w.wake();
        return ready_ == 0 ? awaitable_state<>::ready() : awaitable_state<>::pending();
    }
};

inline auto yield(std::size_t ready = 1) {
    return yield_awaitable(ready);
}

}  // namespace pollcoro
