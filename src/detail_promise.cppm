module;

#if !defined(POLLCORO_IMPORT_STD) || POLLCORO_IMPORT_STD == 0
#include <coroutine>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <optional>
#include <type_traits>
#include <utility>
#endif

export module pollcoro:detail_promise;

#if defined(POLLCORO_IMPORT_STD) && POLLCORO_IMPORT_STD == 1
import std;
#endif

import :allocator;
import :awaitable;
import :stream_awaitable;
import :waker;

export namespace pollcoro::detail {
struct promise_base {
    std::exception_ptr exception{nullptr};

    void* current_awaitable = nullptr;
    bool (*current_awaitable_poll)(void*, const waker&) = nullptr;
};

template<typename T>
class task_storage : public promise_base {
    std::optional<T> result;

  public:
    void return_value(T value) {
        result = std::move(value);
    }

    T take_result() {
        return std::move(*result);
    }
};

template<>
class task_storage<void> : public promise_base {
  public:
    void return_void() {}
};

template<typename T>
class stream_storage : public promise_base {
    std::optional<T> result;

  public:
    void return_void() {}

    template<stream_awaitable StreamAwaitable>
    auto yield_value(StreamAwaitable&& stream_awaitable) {
        using result_type = stream_awaitable_result_t<StreamAwaitable>;

        struct transformed_promise {
            stream_storage& promise;
            StreamAwaitable stream_awaitable;

            constexpr bool await_ready() {
                return false;
            }

            void await_suspend(std::coroutine_handle<>) {
                promise.exception = nullptr;
                promise.current_awaitable = this;
                promise.current_awaitable_poll = [](void* awaitable, const waker& w) {
                    auto self = static_cast<transformed_promise*>(awaitable);
                    auto state = self->stream_awaitable.poll_next(w);
                    if (state.is_done()) {
                        return true;
                    }
                    if (state.is_ready()) {
                        self->promise.yield_value(state.take_result());
                    }

                    return false;
                };
            }

            void await_resume() {
                auto exception = promise.exception;
                promise.current_awaitable = nullptr;
                promise.current_awaitable_poll = nullptr;
                promise.exception = nullptr;
                if (exception) {
                    std::rethrow_exception(exception);
                }
            }
        };

        return transformed_promise{*this, std::move(stream_awaitable)};
    }

    std::suspend_always yield_value(T value) {
        result.emplace(std::move(value));
        return {};
    }

    T take_result() {
        return *std::exchange(result, std::nullopt);
    }

    bool has_value() const {
        return result.has_value();
    }
};

template<typename promise_type, awaitable Awaitable>
auto transform_awaitable(promise_type& promise, Awaitable&& awaitable) {
    using result_type = awaitable_result_t<Awaitable>;

    struct transformed_promise : task_storage<result_type> {
        promise_type& promise;
        Awaitable awaitable;

        constexpr bool await_ready() {
            return false;
        }

        void await_suspend(std::coroutine_handle<>) {
            promise.exception = nullptr;
            promise.current_awaitable = this;
            promise.current_awaitable_poll = [](void* awaitable, const waker& w) {
                auto self = static_cast<transformed_promise*>(awaitable);
                auto state = self->awaitable.poll(w);
                if (state.is_ready()) {
                    if constexpr (!std::is_void_v<result_type>) {
                        self->return_value(state.take_result());
                    }
                    return true;
                } else {
                    return false;
                }
            };
        }

        result_type await_resume() {
            auto exception = promise.exception;
            promise.current_awaitable = nullptr;
            promise.current_awaitable_poll = nullptr;
            promise.exception = nullptr;
            if (exception) {
                std::rethrow_exception(exception);
            }
            if constexpr (!std::is_void_v<result_type>) {
                return this->take_result();
            }
        }
    };

    return transformed_promise{{}, promise, std::move(awaitable)};
}

template<typename task, typename result, typename storage>
struct promise_type : public storage {
    const pollcoro::allocator& alloc_ = default_allocator;

    promise_type() : alloc_(current_allocator()) {}

    ~promise_type() {
#ifndef NDEBUG
        if (this->exception) {
            fprintf(stderr, "Promise destroyed while exception is present\n");
            std::rethrow_exception(this->exception);
        }
#endif
    }

    bool poll_ready(const waker& w) {
        if (this->current_awaitable_poll) {
            return this->current_awaitable_poll(this->current_awaitable, w);
        }
        return true;
    }

    const pollcoro::allocator& allocator() const {
        return alloc_;
    }

    static void* operator new(std::size_t size) {
        return current_allocator().allocate(size);
    }

    static void operator delete(void* ptr) noexcept {}

    task get_return_object() {
        return task(std::coroutine_handle<promise_type>::from_promise(*this));
    }

    std::suspend_always initial_suspend() {
        return {};
    }

    std::suspend_always final_suspend() noexcept {
        return {};
    }

    void unhandled_exception() {
        this->exception = std::current_exception();
    }

    template<awaitable Awaitable>
    auto await_transform(Awaitable&& awaitable) {
        return transform_awaitable<promise_type, std::remove_cvref_t<Awaitable>>(
            *this, std::move(awaitable)
        );
    }
};

}  // namespace pollcoro::detail
