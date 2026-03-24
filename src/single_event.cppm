module;

#if !defined(POLLCORO_IMPORT_STD) || POLLCORO_IMPORT_STD == 0
#include <concepts>
#include <memory>
#include <mutex>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#endif

export module pollcoro:single_event;

#if defined(POLLCORO_IMPORT_STD) && POLLCORO_IMPORT_STD == 1
import std;
#endif

import :awaitable;
import :is_blocking;
import :waker;

export namespace pollcoro {
namespace detail {
template<typename T>
struct single_event_result {
    using type = T;
    std::optional<T> result_;

    void set_result(T result) {
        result_ = std::move(result);
    }

    T take_result() {
        return std::move(*result_);
    }
};

template<>
struct single_event_result<void> {
    using type = std::monostate;

    void set_result(std::monostate) {}
};
}  // namespace detail

template<typename T>
class single_event_awaitable : public awaitable_always_blocks {
    class state : detail::single_event_result<T> {
        using result_type = typename detail::single_event_result<T>::type;

        std::mutex mutex_;
        bool ready_{false};
        waker waker_;

      public:
        void set_waker(const waker& new_waker) {
            std::unique_lock lock(mutex_);
            if (!waker_.will_wake(new_waker)) {
                if (ready_) {
                    lock.unlock();
                    new_waker.wake();
                } else {
                    waker_ = new_waker;
                }
            }
        }

        void mark_ready(result_type new_result) {
            std::unique_lock lock(mutex_);
            auto old_ready = std::exchange(ready_, true);
            if (!old_ready) {
                this->set_result(std::move(new_result));
                auto waker = std::move(waker_);
                lock.unlock();
                waker.wake();
            }
        }

        bool is_ready() const {
            std::unique_lock lock(mutex_);
            return ready_;
        }

        awaitable_state<T> poll_result() {
            std::unique_lock lock(mutex_);
            if (ready_) {
                if constexpr (std::is_void_v<T>) {
                    return awaitable_state<T>::ready();
                } else {
                    return awaitable_state<T>::ready(this->take_result());
                }
            }
            return awaitable_state<T>::pending();
        }
    };

    single_event_awaitable() : state_(std::make_shared<state>()) {}

  public:
    single_event_awaitable(single_event_awaitable&& other)
        : state_(std::move(other.state_)) {}

    single_event_awaitable& operator=(single_event_awaitable&& other) {
        if (this != &other) {
            state_ = std::move(other.state_);
        }
        return *this;
    }

    single_event_awaitable(const single_event_awaitable&) = delete;
    single_event_awaitable& operator=(const single_event_awaitable&) = delete;

    ~single_event_awaitable() {
        if (state_) {
            waker noop;
            state_->set_waker(noop);
        }
    }

    class setter {
      public:
        void set(T value) requires(!std::is_void_v<T>)
        {
            state_->mark_ready(std::move(value));
        }

        void set() requires std::is_void_v<T>
        {
            state_->mark_ready(std::monostate{});
        }

      private:
        explicit setter(std::shared_ptr<state> state) : state_(std::move(state)) {}

        std::shared_ptr<state> state_;
        friend class single_event_awaitable;
    };

    static std::tuple<single_event_awaitable, setter> create() {
        auto awaitable = single_event_awaitable{};
        auto state = awaitable.state_;
        return std::make_tuple(std::move(awaitable), setter(state));
    }

    awaitable_state<T> poll(const waker& w) {
        state_->set_waker(w);
        return state_->poll_result();
    }

  private:
    std::shared_ptr<state> state_;
};

template<typename T>
inline auto single_event() {
    return single_event_awaitable<T>::create();
}

}  // namespace pollcoro
