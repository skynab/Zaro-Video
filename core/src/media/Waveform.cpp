#include "zaro/core/media/Waveform.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "zaro/core/Check.h"

namespace zaro::media {
namespace {

/// Bumped when the layout below changes, so an old cache file is rejected
/// rather than misread.
constexpr std::uint32_t kMagic = 0x5A57'4631;  // "ZWF1"

template <typename T>
void put(std::string& out, const T& value) {
    out.append(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
bool take(const std::string& bytes, std::size_t& offset, T& value) {
    if (offset + sizeof(T) > bytes.size()) {
        return false;
    }
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}

}  // namespace

Waveform::Waveform(std::int32_t channelCount, std::int64_t samplesPerBucket,
                   time::Rational sampleRate)
    : channelCount_{channelCount},
      samplesPerBucket_{samplesPerBucket},
      sampleRate_{std::move(sampleRate)} {
    ZARO_CHECK(channelCount > 0, "a waveform needs at least one channel");
    ZARO_CHECK(samplesPerBucket > 0, "a waveform needs a positive bucket size");
    pending_.assign(static_cast<std::size_t>(channelCount), WaveformBucket{});
}

const WaveformBucket& Waveform::at(std::int32_t channel, std::int64_t bucket) const {
    static const WaveformBucket kSilent{};
    const auto index = static_cast<std::size_t>(bucket * channelCount_ + channel);
    return index < buckets_.size() ? buckets_[index] : kSilent;
}

time::RationalTime Waveform::duration() const {
    return time::RationalTime{bucketCount() * samplesPerBucket_, sampleRate_};
}

void Waveform::append(const AudioBuffer& block) {
    if (!isValid() || !block.isValid()) {
        return;
    }
    const std::int32_t channels = std::min(channelCount_, block.channelCount());

    for (std::int64_t i = 0; i < block.sampleCount(); ++i) {
        for (std::int32_t c = 0; c < channels; ++c) {
            const float sample = block.channel(c)[i];
            WaveformBucket& bucket = pending_[static_cast<std::size_t>(c)];
            if (pendingSamples_ == 0) {
                bucket.minimum = sample;
                bucket.maximum = sample;
            } else {
                bucket.minimum = std::min(bucket.minimum, sample);
                bucket.maximum = std::max(bucket.maximum, sample);
            }
        }
        ++pendingSamples_;

        // Bucket boundaries are counted across calls, so however the audio
        // happens to be blocked, the result is the same.
        if (pendingSamples_ >= samplesPerBucket_) {
            buckets_.insert(buckets_.end(), pending_.begin(), pending_.end());
            std::fill(pending_.begin(), pending_.end(), WaveformBucket{});
            pendingSamples_ = 0;
        }
    }
}

void Waveform::finish() {
    if (pendingSamples_ > 0) {
        buckets_.insert(buckets_.end(), pending_.begin(), pending_.end());
        std::fill(pending_.begin(), pending_.end(), WaveformBucket{});
        pendingSamples_ = 0;
    }
}

Waveform Waveform::resampled(std::int64_t first, std::int64_t last, std::int64_t count) const {
    if (!isValid() || count <= 0) {
        return {};
    }
    first = std::max<std::int64_t>(0, first);
    last = std::min<std::int64_t>(bucketCount(), last);

    Waveform out;
    out.channelCount_ = channelCount_;
    out.sampleRate_ = sampleRate_;
    // Each output bucket covers this many input buckets' worth of samples.
    const std::int64_t span = std::max<std::int64_t>(1, last - first);
    out.samplesPerBucket_ = std::max<std::int64_t>(1, samplesPerBucket_ * span / count);
    out.buckets_.assign(static_cast<std::size_t>(count * channelCount_), WaveformBucket{});

    for (std::int64_t i = 0; i < count; ++i) {
        const std::int64_t from = first + span * i / count;
        const std::int64_t to = std::max(from + 1, first + span * (i + 1) / count);
        for (std::int32_t c = 0; c < channelCount_; ++c) {
            WaveformBucket merged;
            bool started = false;
            for (std::int64_t b = from; b < to && b < last; ++b) {
                const WaveformBucket& source = at(c, b);
                if (!started) {
                    merged = source;
                    started = true;
                } else {
                    merged.minimum = std::min(merged.minimum, source.minimum);
                    merged.maximum = std::max(merged.maximum, source.maximum);
                }
            }
            out.buckets_[static_cast<std::size_t>(i * channelCount_ + c)] = merged;
        }
    }
    return out;
}

std::string Waveform::encode() const {
    std::string out;
    put(out, kMagic);
    put(out, channelCount_);
    put(out, samplesPerBucket_);
    put(out, sampleRate_.num());
    put(out, sampleRate_.den());
    const auto count = static_cast<std::int64_t>(buckets_.size());
    put(out, count);
    out.append(reinterpret_cast<const char*>(buckets_.data()),
               buckets_.size() * sizeof(WaveformBucket));
    return out;
}

Result<Waveform> Waveform::decode(const std::string& bytes) {
    std::size_t offset = 0;
    std::uint32_t magic = 0;
    if (!take(bytes, offset, magic) || magic != kMagic) {
        return Error{ErrorCode::InvalidData, "this is not a waveform cache file"};
    }

    Waveform out;
    std::int64_t rateNum = 0;
    std::int64_t rateDen = 0;
    std::int64_t count = 0;
    if (!take(bytes, offset, out.channelCount_) || !take(bytes, offset, out.samplesPerBucket_) ||
        !take(bytes, offset, rateNum) || !take(bytes, offset, rateDen) ||
        !take(bytes, offset, count)) {
        return Error{ErrorCode::InvalidData, "the waveform cache file is truncated"};
    }
    if (out.channelCount_ <= 0 || out.samplesPerBucket_ <= 0 || rateDen == 0 || count < 0) {
        return Error{ErrorCode::InvalidData, "the waveform cache file is not self-consistent"};
    }
    out.sampleRate_ = time::Rational{rateNum, rateDen};

    const auto bytesNeeded = static_cast<std::size_t>(count) * sizeof(WaveformBucket);
    if (offset + bytesNeeded > bytes.size()) {
        return Error{ErrorCode::InvalidData, "the waveform cache file is truncated"};
    }
    out.buckets_.resize(static_cast<std::size_t>(count));
    std::memcpy(out.buckets_.data(), bytes.data() + offset, bytesNeeded);
    out.pending_.assign(static_cast<std::size_t>(out.channelCount_), WaveformBucket{});
    return out;
}

namespace {

/// Size and a sample of the bytes at each end, mixed with FNV-1a.
///
/// A sample rather than the whole file: reading a hundred gigabytes of camera
/// media on import would make importing unusable.
Result<std::uint64_t> sampledMix(const std::string& path, std::uint64_t& sizeOut) {
    std::error_code code;
    const auto size = std::filesystem::file_size(path, code);
    if (code) {
        return Error{ErrorCode::NotFound, "cannot measure " + path};
    }
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return Error{ErrorCode::NotFound, "cannot open " + path};
    }

    constexpr std::streamsize kSampleBytes = 64 * 1024;
    std::array<char, kSampleBytes> buffer{};
    std::uint64_t mixed = 1469598103934665603ULL;  // FNV-1a offset basis

    const auto absorb = [&mixed](const char* data, std::streamsize length) {
        for (std::streamsize i = 0; i < length; ++i) {
            mixed ^= static_cast<std::uint8_t>(data[i]);
            mixed *= 1099511628211ULL;
        }
    };

    file.read(buffer.data(), kSampleBytes);
    absorb(buffer.data(), file.gcount());

    if (static_cast<std::streamsize>(size) > kSampleBytes * 2) {
        file.clear();
        file.seekg(static_cast<std::streamoff>(size) - kSampleBytes, std::ios::beg);
        file.read(buffer.data(), kSampleBytes);
        absorb(buffer.data(), file.gcount());
    }

    sizeOut = static_cast<std::uint64_t>(size);
    absorb(reinterpret_cast<const char*>(&sizeOut), sizeof(sizeOut));
    return mixed;
}

std::string asText(std::uint64_t mixed) {
    std::array<char, 32> text{};
    std::snprintf(text.data(), text.size(), "%016llx", static_cast<unsigned long long>(mixed));
    return std::string{text.data()};
}

}  // namespace

Result<std::string> quickContentHash(const std::string& path) {
    std::uint64_t size = 0;
    auto mixed = sampledMix(path, size);
    if (!mixed) {
        return mixed.error();
    }
    std::error_code code;
    const auto modified = std::filesystem::last_write_time(path, code);
    if (code) {
        return Error{ErrorCode::NotFound, "cannot read the timestamp of " + path};
    }
    // The timestamp is the difference between this and `contentDigest`: it
    // makes a touched file look different, which is what a cache key wants and
    // what a relink must not have.
    const auto timeValue = static_cast<std::uint64_t>(modified.time_since_epoch().count());
    std::uint64_t withTime = *mixed;
    for (std::size_t i = 0; i < sizeof(timeValue); ++i) {
        withTime ^= static_cast<std::uint8_t>((timeValue >> (i * 8)) & 0xFFULL);
        withTime *= 1099511628211ULL;
    }
    return asText(withTime);
}

Result<std::string> contentDigest(const std::string& path) {
    std::uint64_t size = 0;
    auto mixed = sampledMix(path, size);
    if (!mixed) {
        return mixed.error();
    }
    return asText(*mixed);
}

}  // namespace zaro::media
