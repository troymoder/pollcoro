module;

export module pollcoro:waker;

export namespace pollcoro {
class waker;  // Forward declaration

namespace detail {

template<typename WakerType>
inline void waker_wake_function(void* data) noexcept {
    static_cast<WakerType*>(data)->wake();
}

}  // namespace detail

class waker {
    void (*wake_function_)(void*) = nullptr;
    void* data_ = nullptr;

  public:
    waker() = default;

    waker(void* data, void (*wake_function)(void*))
        : wake_function_(wake_function), data_(data) {}

    template<typename WakerType>
    waker(WakerType& ref) : waker(&ref) {}

    template<typename WakerType>
    waker(WakerType* waker_ptr)
        : wake_function_(&detail::waker_wake_function<WakerType>),
          data_(static_cast<void*>(waker_ptr)) {}

    void wake() const {
        if (wake_function_) {
            wake_function_(data_);
        }
    }

    bool will_wake(const waker& other) const {
        return data_ == other.data_ && wake_function_ == other.wake_function_;
    }
};
}  // namespace pollcoro
