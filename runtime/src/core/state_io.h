// Symmetric save-state serializer: each module has one `X_state(StateIO&)` function
// that both saves and loads, so the two can never drift apart.
#pragma once

#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <type_traits>
#include <vector>

class StateIO {
public:
    explicit StateIO(bool loading) : loading_(loading) {}
    StateIO(const std::vector<uint8_t>& data) : loading_(true), buf_(data) {}

    bool loading() const { return loading_; }
    bool ok() const { return ok_; }
    bool at_end() const { return pos_ >= buf_.size(); }
    std::vector<uint8_t>& data() { return buf_; }

    void raw(void* p, size_t n) {
        if (loading_) {
            if (pos_ + n > buf_.size()) {
                ok_ = false;
                std::memset(p, 0, n);
                return;
            }
            std::memcpy(p, buf_.data() + pos_, n);
        } else {
            const uint8_t* b = static_cast<const uint8_t*>(p);
            buf_.insert(buf_.end(), b, b + n);
        }
        pos_ += n;
    }

    template <class T>
    void operator()(T& v) {
        static_assert(std::is_trivially_copyable_v<T>, "use vec/deq/str for containers");
        raw(&v, sizeof v);
    }

    template <class T>
    void vec(std::vector<T>& v) {
        uint32_t n = (uint32_t)v.size();
        (*this)(n);
        if (loading_) v.resize(n);
        if (n) raw(v.data(), n * sizeof(T));
    }

    template <class T>
    void deq(std::deque<T>& d) {
        uint32_t n = (uint32_t)d.size();
        (*this)(n);
        if (loading_) d.resize(n);
        for (auto& x : d) (*this)(x);
    }

    void str(std::string& s) {
        uint32_t n = (uint32_t)s.size();
        (*this)(n);
        if (loading_) s.resize(n);
        if (n) raw(s.data(), n);
    }

    // Section marker: catches modules saving/loading different amounts of data.
    void marker(uint32_t tag) {
        uint32_t t = tag;
        (*this)(t);
        if (loading_ && t != tag) ok_ = false;
    }

private:
    bool loading_;
    bool ok_ = true;
    std::vector<uint8_t> buf_;
    size_t pos_ = 0;
};
