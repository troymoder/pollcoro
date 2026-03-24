module;

#if !defined(POLLCORO_IMPORT_STD) || POLLCORO_IMPORT_STD == 0
#include <atomic>
#include <coroutine>
#include <exception>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>
#include <variant>
#endif

export module pollcoro:resumable;

#if defined(POLLCORO_IMPORT_STD) && POLLCORO_IMPORT_STD == 1
import std;
#endif

import :awaitable;
import :co_awaitable;
import :waker;

export namespace pollcoro {
namespace detail {
template<typename T>
struct resumable_shared_state {
    using storage = std::conditional_t<std::is_void_v<T>, std::monostate, T>;
    using result_type = T;
    using state_type = pollcoro::awaitable_state<result_type>;

    std::mutex mutex;
    bool done = false;
    pollcoro::waker waker;
    std::variant<storage, std::exception_ptr> result;

    void set_result(storage result) {
        std::lock_guard lock(mutex);
        this->result = std::move(result);
        done = true;
        waker.wake();
    }

    void set_exception(std::exception_ptr exception) {
        std::lock_guard lock(mutex);
        this->result = std::move(exception);
        done = true;
        waker.wake();
    }

    state_type poll(const pollcoro::waker& w) {
        std::lock_guard lock(mutex);
        if (done) {
            if (std::holds_alternative<std::exception_ptr>(result)) {
                std::rethrow_exception(std::get<std::exception_ptr>(result));
            }

            if constexpr (std::is_void_v<result_type>) {
                return state_type::ready();
            } else {
                return state_type::ready(std::move(std::get<storage>(result)));
            }
        }

        waker = w;
        return state_type::pending();
    }
};

template<typename task_t, typename shared_state_t>
class to_pollable_adapter {
    task_t task_;
    std::shared_ptr<shared_state_t> state_;
    bool initialized_ = false;

    using result_type = typename shared_state_t::result_type;
    using state_type = pollcoro::awaitable_state<result_type>;

  public:
    to_pollable_adapter(task_t task, std::shared_ptr<shared_state_t> state)
        : task_(std::move(task)), state_(std::move(state)) {}

    state_type poll(const pollcoro::waker& w) {
        return state_->poll(w);
    }
};

template<typename T = void>
struct resumable_promise_storage {
    std::variant<std::monostate, T, std::exception_ptr> result;

    void return_value(T value) {
        result.template emplace<T>(std::move(value));
    }

    void unhandled_exception() {
        result.template emplace<std::exception_ptr>(std::current_exception());
    }

    T take_result() {
        if (std::holds_alternative<std::exception_ptr>(result)) {
            std::rethrow_exception(std::get<std::exception_ptr>(result));
        }
        return std::move(std::get<T>(result));
    }

    bool has_value() const {
        return !std::holds_alternative<std::monostate>(result);
    }
};

template<>
struct resumable_promise_storage<void> {
    std::variant<std::monostate, std::exception_ptr> result;
    bool completed = false;

    void return_void() {
        completed = true;
    }

    void unhandled_exception() {
        result.template emplace<std::exception_ptr>(std::current_exception());
    }

    void take_result() {
        if (std::holds_alternative<std::exception_ptr>(result)) {
            std::rethrow_exception(std::get<std::exception_ptr>(result));
        }
    }

    bool has_value() const {
        return completed || std::holds_alternative<std::exception_ptr>(result);
    }
};
}  // namespace detail

template<co_awaitable Resumable>
auto to_pollable(Resumable&& resumable) {
    using result_type = co_await_result_t<Resumable>;
    using shared_state_t = detail::resumable_shared_state<result_type>;
    auto state = std::make_shared<shared_state_t>();

    struct task_t {
        struct promise_type {
            task_t get_return_object() {
                return task_t{std::coroutine_handle<promise_type>::from_promise(*this)};
            }

            std::suspend_always initial_suspend() {
                return {};
            }

            std::suspend_always final_suspend() noexcept {
                return {};
            }

            void return_void() {}

            void unhandled_exception() {
                std::terminate();
            }
        };

        std::coroutine_handle<promise_type> handle_;

        task_t(std::coroutine_handle<promise_type> handle) : handle_(handle) {}

        task_t(task_t&& other) : handle_(other.handle_) {
            other.handle_ = nullptr;
        }

        task_t& operator=(task_t&& other) {
            if (this != &other) {
                handle_ = other.handle_;
                other.handle_ = nullptr;
            }
            return *this;
        }

        task_t(const task_t&) = delete;
        task_t& operator=(const task_t&) = delete;

        ~task_t() {
            if (handle_) {
                handle_.destroy();
            }
        }

        void resume() {
            handle_.resume();
        }
    };

    auto task = ([](Resumable resumable, std::shared_ptr<shared_state_t> state) -> task_t {
        try {
            if constexpr (std::is_void_v<result_type>) {
                co_await resumable;
                state->set_result(std::monostate{});
            } else {
                state->set_result(co_await resumable);
            }
        } catch (...) {
            state->set_exception(std::current_exception());
        }
    })(std::move(resumable), state);

    task.resume();

    return detail::to_pollable_adapter<task_t, shared_state_t>(std::move(task), state);
}

template<awaitable Awaitable, co_scheduler Scheduler>
auto to_resumable(Awaitable&& awaitable, Scheduler&& scheduler) {
    using result_type = awaitable_result_t<Awaitable>;

    struct task_t {
        struct promise_type : detail::resumable_promise_storage<result_type> {
            std::coroutine_handle<> continuation;
            std::atomic<bool> resumed{false};

            task_t get_return_object() {
                return task_t{std::coroutine_handle<promise_type>::from_promise(*this)};
            }

            std::suspend_always initial_suspend() noexcept {
                return {};
            }

            auto final_suspend() noexcept {
                struct final_awaiter {
                    bool await_ready() const noexcept {
                        return false;
                    }

                    std::coroutine_handle<>
                    await_suspend(std::coroutine_handle<promise_type> handle) const noexcept {
                        auto cont = handle.promise().continuation;
                        return cont ? cont : std::noop_coroutine();
                    }

                    void await_resume() const noexcept {}
                };

                return final_awaiter{};
            }
        };

        std::coroutine_handle<promise_type> handle_;

        explicit task_t(std::coroutine_handle<promise_type> handle) : handle_(handle) {}

        ~task_t() {
            if (handle_) {
                handle_.destroy();
            }
        }

        task_t(task_t&& other) : handle_(std::exchange(other.handle_, nullptr)) {}

        task_t& operator=(task_t&& other) {
            if (this != &other) {
                if (handle_) {
                    handle_.destroy();
                }
                handle_ = std::exchange(other.handle_, nullptr);
            }
            return *this;
        }

        task_t(const task_t&) = delete;
        task_t& operator=(const task_t&) = delete;

        bool await_ready() const noexcept {
            return handle_.done();
        }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> handle) noexcept {
            handle_.promise().continuation = handle;
            return handle_;
        }

        result_type await_resume() {
            return handle_.promise().take_result();
        }

        void start() {
            handle_.resume();
        }

        bool done() const {
            return handle_.done();
        }
    };

    auto coro = [](std::remove_cvref_t<Awaitable> inner_awaitable,
                   Scheduler&& scheduler) -> task_t {
        struct poll_awaiter {
            Awaitable& awaitable;
            mutable awaitable_state<result_type> state;

            bool await_ready() const noexcept {
                return false;
            }

            bool await_suspend(std::coroutine_handle<typename task_t::promise_type> handle) {
                handle.promise().resumed.store(false, std::memory_order_relaxed);

                state = awaitable.poll(waker(handle.address(), [](void* data) noexcept {
                    auto h =
                        std::coroutine_handle<typename task_t::promise_type>::from_address(data);
                    bool expected = false;
                    if (h.promise().resumed.compare_exchange_strong(
                            expected, true, std::memory_order_acq_rel
                        )) {
                        h.resume();
                    }
                }));

                // If already ready, resume immediately
                if (state.is_ready()) {
                    return false;
                }
                return true;
            }

            awaitable_state<result_type> await_resume() {
                return std::move(state);
            }
        };

        while (true) {
            auto state = co_await poll_awaiter{inner_awaitable, {}};
            if (state.is_ready()) {
                if constexpr (std::is_void_v<result_type>) {
                    co_return;
                } else {
                    co_return state.take_result();
                }
            }
            // Yield to scheduler before next poll
            co_await scheduler.schedule();
        }
    }(std::move(awaitable), std::forward<Scheduler>(scheduler));

    return coro;
}
}  // namespace pollcoro
