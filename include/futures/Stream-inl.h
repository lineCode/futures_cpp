#pragma once

#include <futures/Stream.h>

namespace futures {

template <typename Stream, typename F>
class ForEachFuture : public FutureBase<ForEachFuture<Stream, F>, folly::Unit> {
public:
    using Item = folly::Unit;

    ForEachFuture(Stream &&stream, F&& func)
        : stream_(std::move(stream)), func_(std::move(func)) {
    }

    Poll<Item> poll() override {
        while (true) {
            auto r = stream_.poll();
            if (r.hasException())
                return Poll<Item>(r.exception());
            auto v = folly::moveFromTry(r);
            if (v.isReady()) {
                // End of stream
                if (!v->hasValue()) {
                    return makePollReady(folly::Unit());
                } else {
                    try {
                        func_(std::move(v).value().value());
                    } catch (std::exception &e) {
                        return Poll<Item>(folly::exception_wrapper(std::current_exception(), e));
                    }
                }
            } else {
                return Poll<Item>(not_ready);
            }
        }
    }

private:
    Stream stream_;
    F func_;
};

template <typename T, typename F>
class ForEach2Wrapper {
public:
    ForEach2Wrapper(F&& f)
        : f_(std::move(f)) {
    }

    template <typename T0>
    void operator()(T0&& v) {
        return folly::applyTuple(f_, std::forward<T0>(v));
    }
private:
    F f_;
};

template <typename Derived, typename T>
template <typename F>
ForEachFuture<Derived, F> StreamBase<Derived, T>::forEach(F&& f) {
    return ForEachFuture<Derived, F>(move_self(), std::forward<F>(f));
};

template <typename Derived, typename T>
template <typename F>
ForEachFuture<Derived, ForEach2Wrapper<T, F>>
StreamBase<Derived, T>::forEach2(F&& f) {
    return ForEachFuture<Derived, ForEach2Wrapper<T, F>>(move_self(), std::forward<F>(f));
};

// helper methods
template <typename Iter>
IterStream<Iter> makeIterStream(Iter&& begin, Iter&& end) {
    return IterStream<Iter>(std::forward<Iter>(begin), std::forward<Iter>(end));
}

}