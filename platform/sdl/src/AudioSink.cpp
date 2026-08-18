#include "zaro/platform/sdl/AudioSink.h"

#include <string>

#include <SDL.h>

namespace zaro::platform::sdl {

struct AudioSink::State {
    SDL_AudioDeviceID device{0};
    std::int32_t channels{2};
    std::int32_t bufferFrames{1024};
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

    // Four device buffers of headroom. Less and an ordinary scheduling hiccup
    // on the render thread becomes an audible dropout; much more and a seek
    // takes noticeably long to be heard.
    state.ring = std::make_unique<playback::AudioRingBuffer>(
        channels, static_cast<std::int64_t>(bufferFrames) * 4);

    SDL_AudioSpec wanted{};
    wanted.freq = static_cast<int>(sampleRate.roundToInt());
    wanted.format = AUDIO_F32SYS;
    wanted.channels = static_cast<Uint8>(channels);
    wanted.samples = static_cast<Uint16>(bufferFrames);
    wanted.callback = &audioCallback;
    wanted.userdata = &state;

    SDL_AudioSpec obtained{};
    state.device = SDL_OpenAudioDevice(nullptr, 0, &wanted, &obtained, 0);
    if (state.device == 0) {
        return Error{ErrorCode::Io, std::string{"cannot open an audio device: "} + SDL_GetError()};
    }
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

}  // namespace zaro::platform::sdl
