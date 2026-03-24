module;

#if !defined(POLLCORO_IMPORT_STD) || POLLCORO_IMPORT_STD == 0
#include <coroutine>
#include <exception>
#include <utility>
#endif

export module pollcoro:stream;

#if defined(POLLCORO_IMPORT_STD) && POLLCORO_IMPORT_STD == 1
import std;
#endif

import :detail_promise;
import :is_blocking;
import :stream_awaitable;
import :waker;

export namespace pollcoro {
template<typename T>
class stream : public awaitable_always_blocks {
    void destroy() {
        if (handle_ && destroy_on_drop_) {
            auto& allocator = handle_.promise().allocator();
            auto address = handle_.address();
            handle_.destroy();
            allocator.deallocate(address);
            handle_ = nullptr;
        }
    }

  public:
    using promise_type = detail::promise_type<stream, T, detail::stream_storage<T>>;

    explicit stream(std::coroutine_handle<promise_type> h, bool destroy_on_drop = true)
        : handle_(h), destroy_on_drop_(destroy_on_drop) {}

    stream(stream&& other)
        : handle_(other.handle_), destroy_on_drop_(other.destroy_on_drop_) {
        other.handle_ = nullptr;
    }

    stream& operator=(stream&& other) {
        if (this != &other) {
            destroy();
            handle_ = other.handle_;
            destroy_on_drop_ = other.destroy_on_drop_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    stream(const stream&) = delete;
    stream& operator=(const stream&) = delete;

    ~stream() {
        destroy();
    }

    stream_awaitable_state<T> poll_next(const waker& w) {
        auto& promise = handle_.promise();
        bool resumed = false;
        if (!is_ready()) {
            try {
                resumed = promise.poll_ready(w);
            } catch (...) {
                promise.exception = std::current_exception();
                resumed = true;
            }
            if (resumed) {
                handle_.resume();
            }
        }

        if (promise.has_value()) {
            return stream_awaitable_state<T>::ready(promise.take_result());
        }

        if (is_ready()) {
            return stream_awaitable_state<T>::done();
        }

        if (resumed) {
            w.wake();
        }

        return stream_awaitable_state<T>::pending();
    }

  private:
    std::coroutine_handle<promise_type> handle_;
    bool destroy_on_drop_{true};

    bool is_ready() const {
        return handle_.done();
    }
};

}  // namespace pollcoro
