#include "zaro/core/playback/AudioRingBuffer.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "zaro/core/Check.h"

namespace zaro::playback {

AudioRingBuffer::AudioRingBuffer(std::int32_t channels, std::int64_t capacityFrames)
    : channels_{channels}, capacityFrames_{capacityFrames} {
    ZARO_CHECK(channels > 0, "AudioRingBuffer needs at least one channel");
    ZARO_CHECK(capacityFrames > 0, "AudioRingBuffer needs a positive capacity");
    samples_.assign(static_cast<std::size_t>(capacityFrames) * static_cast<std::size_t>(channels),
                    0.0F);
}

std::int64_t AudioRingBuffer::availableToRead() const {
    return writeTotal_.load(std::memory_order_acquire) - readTotal_.load(std::memory_order_acquire);
}

std::int64_t AudioRingBuffer::availableToWrite() const {
    return capacityFrames_ - availableToRead();
}

std::int64_t AudioRingBuffer::write(const float* interleaved, std::int64_t frames) {
    const std::int64_t writable = std::min(frames, availableToWrite());
    if (writable <= 0) {
        return 0;
    }
    const std::int64_t start = writeTotal_.load(std::memory_order_relaxed) % capacityFrames_;
    const std::int64_t firstRun = std::min(writable, capacityFrames_ - start);

    const auto channels = static_cast<std::size_t>(channels_);
    std::memcpy(samples_.data() + static_cast<std::size_t>(start) * channels, interleaved,
                static_cast<std::size_t>(firstRun) * channels * sizeof(float));
    if (writable > firstRun) {
        std::memcpy(samples_.data(), interleaved + static_cast<std::size_t>(firstRun) * channels,
                    static_cast<std::size_t>(writable - firstRun) * channels * sizeof(float));
    }
    // Release so the consumer cannot observe the advanced counter before the
    // samples it refers to.
    writeTotal_.fetch_add(writable, std::memory_order_release);
    return writable;
}

std::int64_t AudioRingBuffer::read(float* interleaved, std::int64_t frames) {
    const std::int64_t readable = std::min(frames, availableToRead());
    const auto channels = static_cast<std::size_t>(channels_);

    if (readable > 0) {
        const std::int64_t start = readTotal_.load(std::memory_order_relaxed) % capacityFrames_;
        const std::int64_t firstRun = std::min(readable, capacityFrames_ - start);
        std::memcpy(interleaved, samples_.data() + static_cast<std::size_t>(start) * channels,
                    static_cast<std::size_t>(firstRun) * channels * sizeof(float));
        if (readable > firstRun) {
            std::memcpy(interleaved + static_cast<std::size_t>(firstRun) * channels,
                        samples_.data(),
                        static_cast<std::size_t>(readable - firstRun) * channels * sizeof(float));
        }
    }

    if (readable > 0) {
        // The read position advances only by real data, so it can never run
        // past the write position.
        readTotal_.fetch_add(readable, std::memory_order_release);
    }

    if (readable < frames) {
        // Silence, not stale samples. An underrun should be a gap, not a
        // fragment of the last half second played again.
        std::memset(interleaved + static_cast<std::size_t>(readable) * channels, 0,
                    static_cast<std::size_t>(frames - readable) * channels * sizeof(float));
        underrun_.fetch_add(frames - readable, std::memory_order_relaxed);
    }

    // The clock is separate, and counts everything the device was given.
    delivered_.fetch_add(frames, std::memory_order_release);
    return readable;
}

void AudioRingBuffer::reset() {
    writeTotal_.store(0, std::memory_order_relaxed);
    readTotal_.store(0, std::memory_order_relaxed);
    delivered_.store(0, std::memory_order_relaxed);
    underrun_.store(0, std::memory_order_relaxed);
    std::fill(samples_.begin(), samples_.end(), 0.0F);
}

}  // namespace zaro::playback
