#include "zaro/platform/sdl/AudioSink.h"

#include <cstdint>
#include <string>

#include <SDL.h>

namespace zaro::platform::sdl {

struct AudioSink::State {
    SDL_AudioDeviceID device{0};
    std::int32_t channels{2};
    std::int32_t bufferFrames{1024};
    time::Rational sampleRate{time::rates::hz48000};
    std::unique_ptr<playback::AudioRingBuffer> ring;

    ~State() {
        if (device != 0) {
            SDL_CloseAudioDevice(device);
        }
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
};

namespace {

/// Runs on the device's own high-priority thread. It must not allocate, lock or
/// block; reading a lock-free ring is all it is allowed to do.
void audioCallback(void* userData, Uint8* stream, int lengthBytes) {
    auto* state = static_cast<AudioSink::State*>(userData);
    const auto bytesPerFrame =
        static_cast<std::int64_t>(state->channels) * static_cast<std::int64_t>(sizeof(float));
    state->ring->read(reinterpret_cast<float*>(stream),
                      static_cast<std::int64_t>(lengthBytes) / bytesPerFrame);
}

}  // namespace

AudioSink::AudioSink() = default;
AudioSink::~AudioSink() = default;

Result<std::unique_ptr<AudioSink>> AudioSink::open(const time::Rational& sampleRate,
                                                   std::int32_t channels,
                                                   std::int32_t bufferFrames) {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        return Error{ErrorCode::Io, std::string{"cannot start audio: "} + SDL_GetError()};
    }

    auto sink = std::unique_ptr<AudioSink>(new AudioSink());
    sink->state_ = std::make_unique<State>();
    State& state = *sink->state_;
    state.channels = channels;
    state.bufferFrames = bufferFrames;

    SDL_AudioSpec wanted{};
    wanted.freq = static_cast<int>(sampleRate.roundToInt());
    wanted.format = AUDIO_F32SYS;
    wanted.channels = static_cast<Uint8>(channels);
    wanted.samples = static_cast<Uint16>(bufferFrames);
    wanted.callback = &audioCallback;
    wanted.userdata = &state;

    // The rate is the device's to choose. Nothing else is.
    //
    // Rate, because the alternative is worse than accommodating it: a machine
    // set to 44.1kHz will not open a 48kHz stream, and SDL asked to pretend
    // otherwise resamples the finished mix on the way out with a converter
    // nobody chose. Taking the device's rate and mixing at it costs nothing --
    // the mixer takes a rate already, and the decoders resample to whatever
    // they are asked for, which is a resampler we did choose.
    //
    // Not the buffer size, though SDL would happily negotiate that too. A
    // different period is *lossless* -- SDL buffers to bridge it, no samples
    // are harmed -- so there is nothing to gain, and something to lose: the
    // pump's lead and the ring are both multiples of this number, tuned and
    // measured at 1024. Letting the device halve it would quietly halve the
    // headroom that keeps a slow decode from becoming a click.
    //
    // Format and channels stay fixed for a harder reason: the ring holds
    // interleaved stereo floats, and every other line here assumes it.
    SDL_AudioSpec obtained{};
    state.device =
        SDL_OpenAudioDevice(nullptr, 0, &wanted, &obtained, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (state.device == 0) {
        return Error{ErrorCode::Io, std::string{"cannot open an audio device: "} + SDL_GetError()};
    }

    // What came back, not what was asked for. Reporting the request would make
    // every consumer of it -- the clock, the mixer, the pump's lead -- wrong in
    // the same direction at once, and silently.
    if (obtained.freq > 0) {
        state.sampleRate = time::Rational{obtained.freq, 1};
    }

    // Eight device buffers of headroom, which is more than the producer aims to
    // keep filled. The producer's lead is what actually bounds latency; sizing
    // the ring to exactly that lead would leave it unable to write a whole
    // block whenever it was on target, so it would top up in slivers and lose
    // the headroom it was given. The slack above the lead is what lets it
    // refill in blocks.
    //
    // This costs nothing in seek latency: playback stops by pausing the device,
    // and starting again empties the ring rather than playing it out.
    state.ring = std::make_unique<playback::AudioRingBuffer>(
        channels, static_cast<std::int64_t>(state.bufferFrames) * 8);
    return sink;
}

playback::AudioRingBuffer& AudioSink::ring() {
    return *state_->ring;
}

void AudioSink::start() {
    SDL_PauseAudioDevice(state_->device, 0);
}
void AudioSink::pause() {
    SDL_PauseAudioDevice(state_->device, 1);
}

std::int64_t AudioSink::clockFrames() const {
    return state_->ring->framesDelivered();
}
std::int64_t AudioSink::underrunFrames() const {
    return state_->ring->underrunFrames();
}
std::int32_t AudioSink::deviceBufferFrames() const {
    return state_->bufferFrames;
}
const time::Rational& AudioSink::sampleRate() const {
    return state_->sampleRate;
}

}  // namespace zaro::platform::sdl
