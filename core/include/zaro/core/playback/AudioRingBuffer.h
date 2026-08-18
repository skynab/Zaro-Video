#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace zaro::playback {

/// A lock-free single-producer, single-consumer ring of interleaved float
/// samples.
///
/// The consumer is an audio device callback, which runs on a thread that must
/// never block: taking a mutex there risks a priority inversion against the
/// render thread and the result is a click, which is the one artefact an
/// audience always notices. So the buffer is lock free, and the callback's only
/// failure mode is reading silence.
///
/// The read counter is also the playback clock. Samples consumed by the device
/// are the only honest measure of elapsed time -- wall clocks drift against the
/// audio hardware, and a video engine synchronised to a wall clock slowly
/// slides out of lip sync against the sound the audience is actually hearing.
class AudioRingBuffer {
public:
    AudioRingBuffer(std::int32_t channels, std::int64_t capacityFrames);

    [[nodiscard]] std::int32_t channels() const noexcept { return channels_; }
    [[nodiscard]] std::int64_t capacityFrames() const noexcept { return capacityFrames_; }

    /// Producer side. Returns how many frames were actually written, which is
    /// fewer than asked when the buffer is full.
    std::int64_t write(const float* interleaved, std::int64_t frames);

    /// Consumer side. Any shortfall is filled with silence, so the device gets
    /// a full buffer whatever happens, and the shortfall is counted as an
    /// underrun rather than left as whatever was in memory.
    std::int64_t read(float* interleaved, std::int64_t frames);

    [[nodiscard]] std::int64_t availableToRead() const;
    [[nodiscard]] std::int64_t availableToWrite() const;

    /// Total frames handed to the device, silence during an underrun included.
    /// This is the master clock: that time really did pass, and a clock that
    /// stalls exactly when playback is in trouble is worse than useless.
    [[nodiscard]] std::int64_t framesDelivered() const {
        return delivered_.load(std::memory_order_acquire);
    }

    /// Frames of real data taken out of the ring. Never exceeds framesWritten();
    /// this is the read position, and it must not be confused with the clock.
    /// Advancing the position for invented silence would run it past the write
    /// position and corrupt the buffer.
    [[nodiscard]] std::int64_t framesConsumed() const {
        return readTotal_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::int64_t framesWritten() const {
        return writeTotal_.load(std::memory_order_acquire);
    }
    /// Frames of silence the consumer had to invent because the producer fell
    /// behind. Any value above zero is audible.
    [[nodiscard]] std::int64_t underrunFrames() const {
        return underrun_.load(std::memory_order_relaxed);
    }

    void reset();

private:
    std::int32_t channels_;
    std::int64_t capacityFrames_;
    std::vector<float> samples_;

    // Monotonic totals rather than wrapped indices: the difference is the fill
    // level, and there is no ambiguity between full and empty to get wrong.
    std::atomic<std::int64_t> writeTotal_{0};
    std::atomic<std::int64_t> readTotal_{0};
    std::atomic<std::int64_t> delivered_{0};
    std::atomic<std::int64_t> underrun_{0};
};

}  // namespace zaro::playback
