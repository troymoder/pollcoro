module;

#if !defined(POLLCORO_IMPORT_STD) || POLLCORO_IMPORT_STD == 0
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#endif

export module pollcoro:shared_mutex;

#if defined(POLLCORO_IMPORT_STD) && POLLCORO_IMPORT_STD == 1
import std;
#endif

import :awaitable;
import :is_blocking;
import :waker;

export namespace pollcoro {

class shared_mutex;
class shared_lock_guard;
class unique_lock_guard;
class shared_mutex_read_awaitable;
class shared_mutex_write_awaitable;

namespace detail {

struct shared_waiter {
    waker waker_;
    std::atomic<bool> is_ready_{false};
    bool is_writer_{false};
};

struct shared_mutex_state {
    mutable std::mutex mtx_;
    std::size_t readers_{0};
    bool writer_active_{false};
    std::deque<std::shared_ptr<shared_waiter>> waiters_;

    void release_shared() {
        std::unique_lock lock(mtx_);
        --readers_;
        if (readers_ == 0) {
            wake_next(lock);
        }
    }

    void release_exclusive() {
        std::unique_lock lock(mtx_);
        writer_active_ = false;
        wake_next(lock);
    }

    void remove_waiter(const std::shared_ptr<shared_waiter>& waiter) {
        std::unique_lock lock(mtx_);
        auto it = std::find(waiters_.begin(), waiters_.end(), waiter);
        if (it != waiters_.end()) {
            waiters_.erase(it);
        }
    }

  private:
    // Must be called with lock held. May unlock.
    void wake_next(std::unique_lock<std::mutex>& lock) {
        if (waiters_.empty()) {
            return;
        }

        // If front is a writer, wake it exclusively
        if (waiters_.front()->is_writer_) {
            auto next = std::move(waiters_.front());
            waiters_.pop_front();
            next->is_ready_ = true;
            next->waker_.wake();
            lock.unlock();
            return;
        }

        // Front is a reader - wake all consecutive readers
        while (!waiters_.empty() && !waiters_.front()->is_writer_) {
            auto next = std::move(waiters_.front());
            waiters_.pop_front();
            next->is_ready_.store(true, std::memory_order_release);
            next->waker_.wake();
        }
    }
};

}  // namespace detail

/// RAII guard that holds a shared (read) lock. The lock is released when the
/// guard is destroyed or when `unlock()` is called explicitly.
class shared_lock_guard {
    detail::shared_mutex_state* state_;

    explicit shared_lock_guard(detail::shared_mutex_state* state) : state_(state) {}

    friend class shared_mutex_read_awaitable;
    friend class shared_mutex;

  public:
    shared_lock_guard(shared_lock_guard&& other)
        : state_(std::exchange(other.state_, nullptr)) {}

    shared_lock_guard& operator=(shared_lock_guard&& other) {
        if (this != &other) {
            if (state_) {
                state_->release_shared();
            }
            state_ = std::exchange(other.state_, nullptr);
        }
        return *this;
    }

    shared_lock_guard(const shared_lock_guard&) = delete;
    shared_lock_guard& operator=(const shared_lock_guard&) = delete;

    ~shared_lock_guard() {
        if (state_) {
            state_->release_shared();
        }
    }

    /// Explicitly release the lock before the guard is destroyed.
    void unlock() {
        if (state_) {
            state_->release_shared();
            state_ = nullptr;
        }
    }

    /// Check if this guard still holds the lock.
    explicit operator bool() const {
        return state_ != nullptr;
    }
};

/// RAII guard that holds an exclusive (write) lock. The lock is released when
/// the guard is destroyed or when `unlock()` is called explicitly.
class unique_lock_guard {
    detail::shared_mutex_state* state_;

    explicit unique_lock_guard(detail::shared_mutex_state* state) : state_(state) {}

    friend class shared_mutex_write_awaitable;
    friend class shared_mutex;

  public:
    unique_lock_guard(unique_lock_guard&& other)
        : state_(std::exchange(other.state_, nullptr)) {}

    unique_lock_guard& operator=(unique_lock_guard&& other) {
        if (this != &other) {
            if (state_) {
                state_->release_exclusive();
            }
            state_ = std::exchange(other.state_, nullptr);
        }
        return *this;
    }

    unique_lock_guard(const unique_lock_guard&) = delete;
    unique_lock_guard& operator=(const unique_lock_guard&) = delete;

    ~unique_lock_guard() {
        if (state_) {
            state_->release_exclusive();
        }
    }

    /// Explicitly release the lock before the guard is destroyed.
    void unlock() {
        if (state_) {
            state_->release_exclusive();
            state_ = nullptr;
        }
    }

    /// Check if this guard still holds the lock.
    explicit operator bool() const {
        return state_ != nullptr;
    }
};

/// Awaitable that acquires a shared (read) lock. Returns a `shared_lock_guard`
/// when the lock is acquired.
class shared_mutex_read_awaitable : public awaitable_always_blocks {
    detail::shared_mutex_state* state_;
    std::shared_ptr<detail::shared_waiter> waiter_;
    bool registered_{false};

    void deregister() {
        if (registered_ && state_) {
            state_->remove_waiter(waiter_);
            registered_ = false;
        }
    }

    using result_type = shared_lock_guard;
    using state_type = awaitable_state<result_type>;

  public:
    explicit shared_mutex_read_awaitable(detail::shared_mutex_state* state)
        : state_(state), waiter_(std::make_shared<detail::shared_waiter>()) {
        waiter_->is_writer_ = false;
    }

    shared_mutex_read_awaitable(shared_mutex_read_awaitable&& other)
        : state_(std::exchange(other.state_, nullptr)),
          waiter_(std::move(other.waiter_)),
          registered_(std::exchange(other.registered_, false)) {}

    shared_mutex_read_awaitable& operator=(shared_mutex_read_awaitable&& other) {
        if (this != &other) {
            deregister();
            state_ = std::exchange(other.state_, nullptr);
            waiter_ = std::move(other.waiter_);
            registered_ = std::exchange(other.registered_, false);
        }
        return *this;
    }

    shared_mutex_read_awaitable(const shared_mutex_read_awaitable&) = delete;
    shared_mutex_read_awaitable& operator=(const shared_mutex_read_awaitable&) = delete;

    ~shared_mutex_read_awaitable() {
        deregister();
    }

    state_type poll(const waker& w) {
        // Fast check if the waiter is ready
        if (waiter_->is_ready_.load(std::memory_order_acquire)) {
            return state_type::ready(shared_lock_guard(std::exchange(state_, nullptr)));
        }

        std::unique_lock lock(state_->mtx_);
        waiter_->waker_ = w;

        if (!registered_) {
            // Can acquire immediately if no writer active and no writers waiting
            // (writer-preference to prevent writer starvation)
            bool has_waiting_writer =
                std::any_of(state_->waiters_.begin(), state_->waiters_.end(), [](const auto& w) {
                    return w->is_writer_;
                });

            if (!state_->writer_active_ && !has_waiting_writer) {
                ++state_->readers_;
                return state_type::ready(shared_lock_guard(std::exchange(state_, nullptr)));
            }

            registered_ = true;
            state_->waiters_.push_back(waiter_);
            return state_type::pending();
        }

        if (!waiter_->is_ready_) {
            return state_type::pending();
        }

        ++state_->readers_;
        registered_ = false;
        return state_type::ready(shared_lock_guard(state_));
    }
};

/// Awaitable that acquires an exclusive (write) lock. Returns a
/// `unique_lock_guard` when the lock is acquired.
class shared_mutex_write_awaitable : public awaitable_always_blocks {
    detail::shared_mutex_state* state_;
    std::shared_ptr<detail::shared_waiter> waiter_;
    bool registered_{false};

    void deregister() {
        if (registered_ && state_) {
            state_->remove_waiter(waiter_);
            registered_ = false;
        }
    }

    using result_type = unique_lock_guard;
    using state_type = awaitable_state<result_type>;

  public:
    explicit shared_mutex_write_awaitable(detail::shared_mutex_state* state)
        : state_(state), waiter_(std::make_shared<detail::shared_waiter>()) {
        waiter_->is_writer_ = true;
    }

    shared_mutex_write_awaitable(shared_mutex_write_awaitable&& other)
        : state_(std::exchange(other.state_, nullptr)),
          waiter_(std::move(other.waiter_)),
          registered_(std::exchange(other.registered_, false)) {}

    shared_mutex_write_awaitable& operator=(shared_mutex_write_awaitable&& other) {
        if (this != &other) {
            deregister();
            state_ = std::exchange(other.state_, nullptr);
            waiter_ = std::move(other.waiter_);
            registered_ = std::exchange(other.registered_, false);
        }
        return *this;
    }

    shared_mutex_write_awaitable(const shared_mutex_write_awaitable&) = delete;
    shared_mutex_write_awaitable& operator=(const shared_mutex_write_awaitable&) = delete;

    ~shared_mutex_write_awaitable() {
        deregister();
    }

    state_type poll(const waker& w) {
        // Fast check if the waiter is ready
        if (waiter_->is_ready_.load(std::memory_order_acquire)) {
            return state_type::ready(unique_lock_guard(std::exchange(state_, nullptr)));
        }

        std::unique_lock lock(state_->mtx_);
        waiter_->waker_ = w;

        if (!registered_) {
            if (!state_->writer_active_ && state_->readers_ == 0) {
                state_->writer_active_ = true;
                return state_type::ready(unique_lock_guard(std::exchange(state_, nullptr)));
            }

            registered_ = true;
            state_->waiters_.push_back(waiter_);
            return state_type::pending();
        }

        if (!waiter_->is_ready_) {
            return state_type::pending();
        }

        state_->writer_active_ = true;
        registered_ = false;
        return state_type::ready(unique_lock_guard(std::exchange(state_, nullptr)));
    }
};

/// An async shared mutex (read-write lock) that can be held across yield points.
///
/// This mutex allows multiple concurrent readers or a single exclusive writer.
/// It uses writer-preference scheduling to prevent writer starvation: when a
/// writer is waiting, new readers will queue behind it.
///
/// Example:
/// ```cpp
/// pollcoro::shared_mutex mtx;
///
/// task<void> reader() {
///     auto guard = co_await mtx.lock_shared();
///     // read-only access - multiple readers can be here concurrently
///     co_await some_async_read();
/// }
///
/// task<void> writer() {
///     auto guard = co_await mtx.lock();
///     // exclusive access - no other readers or writers
///     co_await some_async_write();
/// }
/// ```
class shared_mutex {
    detail::shared_mutex_state state_;

  public:
    shared_mutex() = default;

    /// Returns an awaitable that acquires an exclusive (write) lock.
    /// The returned awaitable yields a `unique_lock_guard` that releases the
    /// lock when destroyed.
    shared_mutex_write_awaitable lock() {
        return shared_mutex_write_awaitable(&state_);
    }

    /// Returns an awaitable that acquires a shared (read) lock.
    /// The returned awaitable yields a `shared_lock_guard` that releases the
    /// lock when destroyed.
    shared_mutex_read_awaitable lock_shared() {
        return shared_mutex_read_awaitable(&state_);
    }

    /// Attempts to acquire an exclusive (write) lock immediately without
    /// blocking. Returns a `unique_lock_guard` if successful, or `std::nullopt`
    /// if the lock is currently held.
    std::optional<unique_lock_guard> try_lock() {
        std::unique_lock lock(state_.mtx_);
        if (!state_.writer_active_ && state_.readers_ == 0) {
            state_.writer_active_ = true;
            return unique_lock_guard(&state_);
        }
        return std::nullopt;
    }

    /// Attempts to acquire a shared (read) lock immediately without blocking.
    /// Returns a `shared_lock_guard` if successful, or `std::nullopt` if a
    /// writer currently holds the lock or is waiting.
    std::optional<shared_lock_guard> try_lock_shared() {
        std::unique_lock lock(state_.mtx_);
        bool has_waiting_writer =
            std::any_of(state_.waiters_.begin(), state_.waiters_.end(), [](const auto& w) {
                return w->is_writer_;
            });

        if (!state_.writer_active_ && !has_waiting_writer) {
            ++state_.readers_;
            return shared_lock_guard(&state_);
        }
        return std::nullopt;
    }

    /// Returns the number of readers currently holding the lock.
    std::size_t reader_count() const {
        std::unique_lock lock(state_.mtx_);
        return state_.readers_;
    }

    /// Check if a writer currently holds the lock.
    bool is_writer_active() const {
        std::unique_lock lock(state_.mtx_);
        return state_.writer_active_;
    }
};

}  // namespace pollcoro
