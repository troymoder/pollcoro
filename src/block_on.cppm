module;

#if !defined(POLLCORO_IMPORT_STD) || POLLCORO_IMPORT_STD == 0
#include <condition_variable>
#include <mutex>
#include <type_traits>
#endif

export module pollcoro:block_on;

#if defined(POLLCORO_IMPORT_STD) && POLLCORO_IMPORT_STD == 1
import std;
#endif

import :awaitable;
import :is_blocking;
import :waker;

export namespace pollcoro {
template<awaitable Awaitable>
auto block_on(Awaitable&& awaitable) -> awaitable_result_t<std::remove_cvref_t<Awaitable>> {
    if constexpr (!is_blocking_v<Awaitable>) {
        while (true) {
            auto result = awaitable.poll(waker());
            if (result.is_ready()) {
                return result.take_result();
            }
        }
    }

    struct waker_data_t {
        std::mutex mutex;
        std::condition_variable cv;
        bool notified = false;

        void wake() noexcept {
            {
                std::lock_guard lock(mutex);
                notified = true;
            }
            cv.notify_all();
        }
    };

    waker_data_t wd;
    while (true) {
        std::unique_lock lock(wd.mutex);
        wd.notified = false;
        lock.unlock();

        auto result = awaitable.poll(waker(wd));
        if (result.is_ready()) {
            return result.take_result();
        }

        lock.lock();
        wd.cv.wait(lock, [&] {
            return wd.notified;
        });
    }
}

}  // namespace pollcoro
