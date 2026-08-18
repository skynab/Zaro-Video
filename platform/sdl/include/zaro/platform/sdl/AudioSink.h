#pragma once

#include <memory>

#include "zaro/core/Error.h"
#include "zaro/core/playback/AudioRingBuffer.h"
#include "zaro/core/time/Rational.h"

namespace zaro::platform::sdl {

/// An audio output device, and the playback clock that comes with it.
///
/// The device callback is the only consumer of the ring buffer, and the count
/// of samples it has taken is what the rest of playback treats as "now". A wall
/// clock would drift against the audio hardware -- a card running 0.01% fast
/// puts picture a frame out every three minutes -- and the audience hears the
/// audio, not the wall clock.
class AudioSink {
public:
    static Result<std::unique_ptr<AudioSink>> open(const time::Rational& sampleRate,
                                                   std::int32_t channels,
                                                   std::int32_t bufferFrames = 1024);
    ~AudioSink();

    /// Where the producer writes mixed audio.
    [[nodiscard]] playback::AudioRingBuffer& ring();

    void start();
    void pause();

    /// Samples delivered to the device: the master clock.
    [[nodiscard]] std::int64_t clockFrames() const;
    /// Samples of silence the device had to be given because the producer fell
    /// behind. Any value above zero was audible.
    [[nodiscard]] std::int64_t underrunFrames() const;

    [[nodiscard]] std::int32_t deviceBufferFrames() const;

    /// Public only because the device callback, which is a free function with
    /// C linkage, needs to reach it. Not part of the interface.
    struct State;

private:
    AudioSink();

    std::unique_ptr<State> state_;
};

}  // namespace zaro::platform::sdl
