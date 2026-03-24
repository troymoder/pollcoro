module;

#if !defined(POLLCORO_IMPORT_STD) || POLLCORO_IMPORT_STD == 0
#include <concepts>
#include <functional>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>
#endif

export module pollcoro:sleep;

#if defined(POLLCORO_IMPORT_STD) && POLLCORO_IMPORT_STD == 1
import std;
#endif

import :awaitable;
import :is_blocking;
import :waker;

export namespace pollcoro {
template<typename Timer>
concept timer = requires(Timer timer) {
    typename Timer::duration;
    typename Timer::time_point;
    { timer.now() } -> std::same_as<typename Timer::time_point>;
    {
        timer.now() + std::declval<typename Timer::duration>()
    } -> std::same_as<typename Timer::time_point>;
    {
        timer.register_callback(
            std::declval<const typename Timer::time_point&>(), std::declval<std::function<void()>>()
        )
    } -> std::same_as<void>;
};

template<timer Timer>
class sleep_awaitable : public awaitable_always_blocks {
    struct shared {
        std::mutex mutex;
        pollcoro::waker waker;
    };

    std::shared_ptr<shared> shared_;
    bool started_ = false;
    Timer timer_;
    typename Timer::time_point deadline_;

    void reset() {
        if (shared_ && started_) {
            std::lock_guard lock(shared_->mutex);
            shared_->waker = pollcoro::waker();
        }
        shared_ = nullptr;
        started_ = false;
    }

  public:
    template<typename T = Timer>
    sleep_awaitable(typename Timer::time_point deadline, T&& timer)
        : timer_(std::forward<T>(timer)),
          deadline_(deadline),
          shared_(std::make_shared<shared>()) {}

    ~sleep_awaitable() {
        reset();
    }

    sleep_awaitable(const sleep_awaitable& other) = delete;
    sleep_awaitable& operator=(const sleep_awaitable& other) = delete;

    sleep_awaitable(sleep_awaitable&& other) {
        shared_ = std::move(other.shared_);
        started_ = other.started_;
        deadline_ = other.deadline_;
        timer_ = std::move(other.timer_);
        other.shared_ = nullptr;
        other.started_ = false;
    }

    sleep_awaitable& operator=(sleep_awaitable&& other) {
        if (this != &other) {
            reset();
            shared_ = std::move(other.shared_);
            started_ = other.started_;
            deadline_ = other.deadline_;
            timer_ = std::move(other.timer_);
            other.shared_ = nullptr;
            other.started_ = false;
        }
        return *this;
    }

    awaitable_state<> poll(const waker& w) {
        if (timer_.now() >= deadline_) {
            return awaitable_state<>::ready();
        }

        std::lock_guard lock(shared_->mutex);
        shared_->waker = w;
        if (!started_) {
            started_ = true;
            timer_.register_callback(deadline_, [shared = shared_]() {
                std::lock_guard lock(shared->mutex);
                shared->waker.wake();
            });
        }

        return awaitable_state<>::pending();
    }
};

template<timer Timer>
auto sleep_for(typename Timer::duration duration) {
    auto timer = Timer();
    auto deadline = timer.now() + duration;
    return sleep_awaitable<Timer>(deadline, std::move(timer));
}

template<timer Timer>
auto sleep_until(typename Timer::time_point deadline) {
    return sleep_awaitable<Timer>(deadline, Timer());
}

template<timer Timer>
auto sleep_for(typename Timer::duration duration, Timer&& timer) {
    auto deadline = timer.now() + duration;
    return sleep_awaitable<Timer>(deadline, std::forward<Timer>(timer));
}

template<timer Timer>
auto sleep_until(typename Timer::time_point deadline, Timer&& timer) {
    return sleep_awaitable<Timer>(deadline, std::forward<Timer>(timer));
}
}  // namespace pollcoro
