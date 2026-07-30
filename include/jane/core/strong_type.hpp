#pragma once

#include <concepts>
#include <cstddef>
#include <functional>
#include <ostream>

// Zero-cost strong types for the engine's core vocabulary (Price, Quantity,
// OrderId, ...). The goal is to make it a *compile error* to pass a
// Quantity where a Price is expected, or to add an OrderId to a Sequence —
// mistakes that are easy to make and silent to miss when everything is
// just an int64_t. Every wrapper below compiles down to operations on the
// underlying integer; there is no runtime cost.
//
// Two flavors are provided because they support different operations:
//   - StrongId:     comparable + hashable identifiers (OrderId, SymbolId, ...).
//                    No arithmetic — adding two OrderIds is never meaningful.
//   - StrongAmount: comparable + arithmetic measures (Price, Quantity, ...).
//                    Supports +, -, unary -, and scaling by the underlying type.

namespace jane {

template <std::regular Underlying, typename Tag>
class StrongId {
public:
    using value_type = Underlying;

    constexpr StrongId() noexcept = default;
    constexpr explicit StrongId(Underlying v) noexcept : value_(v) {}

    [[nodiscard]] constexpr Underlying value() const noexcept { return value_; }

    friend constexpr auto operator<=>(const StrongId&, const StrongId&) noexcept = default;

private:
    Underlying value_{};
};

template <std::regular Underlying, typename Tag>
std::ostream& operator<<(std::ostream& os, StrongId<Underlying, Tag> id) {
    return os << id.value();
}

template <std::regular Underlying, typename Tag>
class StrongAmount {
public:
    using value_type = Underlying;

    constexpr StrongAmount() noexcept = default;
    constexpr explicit StrongAmount(Underlying v) noexcept : value_(v) {}

    [[nodiscard]] constexpr Underlying value() const noexcept { return value_; }

    friend constexpr auto operator<=>(const StrongAmount&, const StrongAmount&) noexcept = default;

    constexpr StrongAmount& operator+=(StrongAmount rhs) noexcept {
        value_ += rhs.value_;
        return *this;
    }
    constexpr StrongAmount& operator-=(StrongAmount rhs) noexcept {
        value_ -= rhs.value_;
        return *this;
    }
    friend constexpr StrongAmount operator+(StrongAmount a, StrongAmount b) noexcept { return a += b; }
    friend constexpr StrongAmount operator-(StrongAmount a, StrongAmount b) noexcept { return a -= b; }
    friend constexpr StrongAmount operator-(StrongAmount a) noexcept { return StrongAmount{-a.value_}; }

    constexpr StrongAmount& operator*=(Underlying scalar) noexcept {
        value_ *= scalar;
        return *this;
    }
    friend constexpr StrongAmount operator*(StrongAmount a, Underlying s) noexcept { return a *= s; }
    friend constexpr StrongAmount operator*(Underlying s, StrongAmount a) noexcept { return a *= s; }

private:
    Underlying value_{};
};

template <std::regular Underlying, typename Tag>
std::ostream& operator<<(std::ostream& os, StrongAmount<Underlying, Tag> a) {
    return os << a.value();
}

}  // namespace jane

template <std::regular Underlying, typename Tag>
struct std::hash<jane::StrongId<Underlying, Tag>> {
    [[nodiscard]] std::size_t operator()(jane::StrongId<Underlying, Tag> id) const noexcept {
        return std::hash<Underlying>{}(id.value());
    }
};

template <std::regular Underlying, typename Tag>
struct std::hash<jane::StrongAmount<Underlying, Tag>> {
    [[nodiscard]] std::size_t operator()(jane::StrongAmount<Underlying, Tag> a) const noexcept {
        return std::hash<Underlying>{}(a.value());
    }
};
