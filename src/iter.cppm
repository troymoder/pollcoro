module;

#if !defined(POLLCORO_IMPORT_STD) || POLLCORO_IMPORT_STD == 0
#include <iterator>
#include <type_traits>
#include <utility>
#endif

#if defined(_MSC_VER)
#define POLLCORO_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#else
#define POLLCORO_NO_UNIQUE_ADDRESS [[no_unique_address]]
#endif

export module pollcoro:iter;

#if defined(POLLCORO_IMPORT_STD) && POLLCORO_IMPORT_STD == 1
import std;
#endif

import :is_blocking;
import :stream_awaitable;
import :waker;

export namespace pollcoro {
namespace detail {
enum class iter_mode {
    copy,
    move
};

struct empty_storage {};

template<typename Iterator, iter_mode Mode, typename Storage = void>
class iter_stream_awaitable : public awaitable_never_blocks {
    using value_type = typename std::iterator_traits<Iterator>::value_type;
    using state_type = stream_awaitable_state<value_type>;
    static constexpr bool owns_storage = !std::is_void_v<Storage>;
    using storage_type = std::conditional_t<owns_storage, Storage, empty_storage>;

    POLLCORO_NO_UNIQUE_ADDRESS storage_type storage_;
    Iterator current_;
    Iterator end_;

    template<typename S = Storage>
    requires(!std::is_void_v<S>)
    iter_stream_awaitable(
        typename std::iterator_traits<Iterator>::difference_type offset,
        iter_stream_awaitable&& other
    )
        : storage_(std::move(other.storage_)) {
        current_ = std::next(std::begin(storage_), offset);
        end_ = std::end(storage_);
    }

  public:
    // Non-owning constructor (iterator pair)
    template<typename S = Storage>
    requires std::is_void_v<S>
    iter_stream_awaitable(Iterator begin, Iterator end)
        : storage_{}, current_(std::move(begin)), end_(std::move(end)) {}

    // Owning constructor (container)
    template<typename S = Storage>
    requires(!std::is_void_v<S>)
    iter_stream_awaitable(S container) : storage_(std::move(container)) {
        current_ = std::begin(storage_);
        end_ = std::end(storage_);
    }

    // Non-owning move constructor
    template<typename S = Storage>
    requires std::is_void_v<S>
    iter_stream_awaitable(iter_stream_awaitable&& other)
        : storage_{}, current_(std::move(other.current_)), end_(std::move(other.end_)) {}

    // Owning move constructor
    template<typename S = Storage>
    requires(!std::is_void_v<S>)
    iter_stream_awaitable(iter_stream_awaitable&& other)
        : iter_stream_awaitable(
              std::distance(std::begin(other.storage_), other.current_), std::move(other)
          ) {}

    iter_stream_awaitable& operator=(iter_stream_awaitable&& other) {
        if (this != &other) {
            auto offset = std::distance(std::begin(other.storage_), other.current_);
            storage_ = std::move(other.storage_);
            current_ = std::next(std::begin(storage_), offset);
            end_ = std::end(storage_);
        }

        return *this;
    }

    iter_stream_awaitable(const iter_stream_awaitable&) = delete;
    iter_stream_awaitable& operator=(const iter_stream_awaitable&) = delete;

    constexpr state_type poll_next(const waker&) {
        if (current_ == end_) {
            return state_type::done();
        }
        if constexpr (Mode == iter_mode::move) {
            return state_type::ready(std::move(*current_++));
        } else {
            return state_type::ready(*current_++);
        }
    }
};
}  // namespace detail

// From iterator pair (copy)
template<typename Iterator>
constexpr auto iter(Iterator begin, Iterator end) {
    return detail::iter_stream_awaitable<Iterator, detail::iter_mode::copy>(begin, end);
}

// From rvalue range (copy, owning)
template<typename Storage>
requires requires(Storage& s) { std::begin(s); std::end(s); }
constexpr auto iter(Storage storage) {
    using Iter = decltype(std::begin(std::declval<Storage&>()));
    return detail::iter_stream_awaitable<Iter, detail::iter_mode::copy, Storage>(
        std::move(storage)
    );
}

// From iterator pair (move)
template<typename Iterator>
constexpr auto iter_move(Iterator begin, Iterator end) {
    return detail::iter_stream_awaitable<Iterator, detail::iter_mode::move>(begin, end);
}

// From rvalue range (move elements, owning container)
template<typename Storage>
requires requires(Storage& s) { std::begin(s); std::end(s); }
constexpr auto iter_move(Storage storage) {
    using Iter = decltype(std::begin(std::declval<Storage&>()));
    return detail::iter_stream_awaitable<Iter, detail::iter_mode::move, Storage>(
        std::move(storage)
    );
}

}  // namespace pollcoro
