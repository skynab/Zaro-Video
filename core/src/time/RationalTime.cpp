#include "zaro/core/time/RationalTime.h"

#include <cassert>

namespace zaro::time {

RationalTime RationalTime::fromSeconds(const Rational& seconds, const Rational& rate) {
    assert(rate.isPositive() && "rate must be positive");
    return RationalTime{(seconds * rate).roundToInt(), rate};
}

Rational RationalTime::toSeconds() const {
    assert(rate_.isPositive() && "rate must be positive");
    return Rational::fromInt(frames_) / rate_;
}

RationalTime RationalTime::rescaledTo(const Rational& newRate) const {
    if (newRate == rate_) {
        return *this;
    }
    return fromSeconds(toSeconds(), newRate);
}

RationalTime RationalTime::abs() const {
    return frames_ < 0 ? RationalTime{-frames_, rate_} : *this;
}

RationalTime& RationalTime::operator+=(const RationalTime& rhs) {
    if (rate_ == rhs.rate_) {
        frames_ += rhs.frames_;
        return *this;
    }
    const Rational& finer = rate_ > rhs.rate_ ? rate_ : rhs.rate_;
    const RationalTime lhs = rescaledTo(finer);
    const RationalTime other = rhs.rescaledTo(finer);
    frames_ = lhs.frames_ + other.frames_;
    rate_ = finer;
    return *this;
}

RationalTime& RationalTime::operator-=(const RationalTime& rhs) {
    return *this += -rhs;
}

bool operator==(const RationalTime& lhs, const RationalTime& rhs) noexcept {
    if (lhs.rate_ == rhs.rate_) {
        return lhs.frames_ == rhs.frames_;
    }
    return lhs.toSeconds() == rhs.toSeconds();
}

std::strong_ordering operator<=>(const RationalTime& lhs, const RationalTime& rhs) noexcept {
    if (lhs.rate_ == rhs.rate_) {
        return lhs.frames_ <=> rhs.frames_;
    }
    return lhs.toSeconds() <=> rhs.toSeconds();
}

std::string RationalTime::toString() const {
    return std::to_string(frames_) + "@" + rate_.toString();
}

}  // namespace zaro::time
