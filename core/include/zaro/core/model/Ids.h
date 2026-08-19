#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace zaro::model {

/// A stable, opaque identifier.
///
/// Phantom-tagged so a ClipId cannot be passed where a TrackId is wanted --
/// both are 64-bit integers, and in a model where clips, tracks and sequences
/// are all addressed by number, that mistake is otherwise silent and constant.
///
/// Zero is reserved to mean "none", so a default-constructed id is invalid
/// rather than accidentally referring to the first thing created.
template <typename Tag>
class Id {
public:
    constexpr Id() noexcept = default;
    explicit constexpr Id(std::uint64_t value) noexcept : value_{value} {}

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool isValid() const noexcept { return value_ != 0; }
    explicit constexpr operator bool() const noexcept { return isValid(); }

    friend constexpr bool operator==(Id, Id) noexcept = default;
    friend constexpr auto operator<=>(Id, Id) noexcept = default;

private:
    std::uint64_t value_{0};
};

using ClipId = Id<struct ClipTag>;
using TrackId = Id<struct TrackTag>;
using SequenceId = Id<struct SequenceTag>;
using MediaRefId = Id<struct MediaRefTag>;
using TransitionId = Id<struct TransitionTag>;
using LinkId = Id<struct LinkTag>;

/// Hands out ids that stay unique for the life of a project, including across
/// save and load: loading restores the counter past the highest id seen, so a
/// reopened project never reissues an id that something still points at.
class IdGenerator {
public:
    template <typename Tag>
    [[nodiscard]] Id<Tag> next() noexcept {
        return Id<Tag>{next_++};
    }

    /// Called after loading, with the largest id found in the file.
    void observe(std::uint64_t existing) noexcept {
        if (existing >= next_) {
            next_ = existing + 1;
        }
    }

    [[nodiscard]] std::uint64_t peek() const noexcept { return next_; }
    void reset() noexcept { next_ = 1; }

private:
    std::uint64_t next_{1};
};

}  // namespace zaro::model

namespace std {
template <typename Tag>
struct hash<zaro::model::Id<Tag>> {
    std::size_t operator()(zaro::model::Id<Tag> id) const noexcept {
        return std::hash<std::uint64_t>{}(id.value());
    }
};
}  // namespace std
