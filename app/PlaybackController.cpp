#include "PlaybackController.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

namespace zaro::app {

PlaybackController::PlaybackController(QObject* parent) : QObject{parent} {
    clockTimer_ = new QTimer(this);
    // Faster than any display refresh, so the playhead is never waiting on the
    // timer rather than on the clock.
    clockTimer_->setInterval(4);
    connect(clockTimer_, &QTimer::timeout, this, [this] { followClock(); });
}

PlaybackController::~PlaybackController() {
    shutDown();
}

void PlaybackController::setSource(const model::Sequence* sequence,
                                   platform::ffmpeg::ProjectMediaSource* media) {
    sequence_ = sequence;
    media_ = media;
}

void PlaybackController::togglePlay(const time::RationalTime& from) {
    if (playing_) {
        transport_.pause();
        stop();
    } else {
        transport_.play();
        startIfPlaying(from);
    }
}

void PlaybackController::startIfPlaying(const time::RationalTime& from) {
    if (sequence_ == nullptr || transport_.speed().isZero()) {
        stop();
        return;
    }
    if (playing_) {
        // Already running; the new speed is picked up from the transport on the
        // next tick, re-anchored so the playhead does not jump.
        anchorPosition_ = from;
        anchorClock_ = sink_ ? sink_->clockFrames() : 0;
        return;
    }
    startClock(from);
}

void PlaybackController::stop() {
    if (!playing_) {
        return;
    }
    playing_ = false;
    clockTimer_->stop();
    audioRunning_.store(false, std::memory_order_relaxed);
    if (audioThread_.joinable()) {
        audioThread_.join();
    }
    if (sink_) {
        sink_->pause();
    }
    emit playingChanged(false);
}

void PlaybackController::shutDown() {
    stop();
}

render::AudioGraph::Meters PlaybackController::meters() const {
    const std::lock_guard<std::mutex> guard{meterMutex_};
    return latestMeters_;
}

void PlaybackController::startClock(const time::RationalTime& from) {
    const time::Rational& audioRate = sequence_->audioSampleRate();
    if (!sink_) {
        auto opened = platform::sdl::AudioSink::open(audioRate, 2);
        if (opened) {
            sink_ = std::move(*opened);
        }
    }
    anchorPosition_ = from;
    anchorClock_ = sink_ ? sink_->clockFrames() : 0;
    audioWritten_ = sink_ ? sink_->clockFrames() : 0;
    playing_ = true;
    emit playingChanged(true);

    if (sink_) {
        // Audio on its own thread, for the reason ADR-006 records: sharing
        // one with rendering starves the device the moment a frame is slow.
        audioRunning_.store(true, std::memory_order_relaxed);
        audioThread_ = std::thread{[this] { pumpAudio(); }};
        sink_->start();
    }
    clockTimer_->start();
}

void PlaybackController::pumpAudio() {
    render::AudioGraph mixer{*media_};
    // A compressor's envelope and a filter's delay line are state, so
    // starting playback somewhere new has to begin from silence -- else the
    // first moment after a jump is ducked by whatever was loud wherever the
    // playhead was before.
    mixer.resetProcessing();
    const time::Rational& audioRate = sequence_->audioSampleRate();
    constexpr std::int32_t kChannels = 2;

    while (audioRunning_.load(std::memory_order_relaxed)) {
        const std::int64_t target = sink_->clockFrames() + sink_->deviceBufferFrames() * 3;
        while (audioWritten_ < target && audioRunning_.load(std::memory_order_relaxed)) {
            const std::int64_t block = std::min<std::int64_t>(1024, target - audioWritten_);
            if (sink_->ring().availableToWrite() < block) {
                break;
            }
            std::vector<float> interleaved(static_cast<std::size_t>(block * kChannels), 0.0F);

            // Only at 1x: shuttling needs pitch handling to sound like
            // anything, so it runs silent while the clock keeps time.
            if (transport_.speed() == time::Rational::fromInt(1)) {
                const auto from = anchorPosition_.rescaledTo(audioRate) +
                                  time::RationalTime{audioWritten_ - anchorClock_, audioRate};
                if (auto mixed = mixer.mix(*sequence_, from, block, kChannels)) {
                    {
                        // Copied under a lock for the UI to read. This
                        // thread already allocates a buffer per block, so
                        // it is not a realtime context -- the realtime path
                        // is the device callback draining the ring.
                        const std::lock_guard<std::mutex> guard{meterMutex_};
                        latestMeters_ = mixer.meters();
                    }
                    for (std::int64_t i = 0; i < mixed->sampleCount(); ++i) {
                        for (std::int32_t c = 0; c < kChannels; ++c) {
                            interleaved[static_cast<std::size_t>(i * kChannels + c)] =
                                mixed->channel(c)[i];
                        }
                    }
                }
            }
            audioWritten_ += sink_->ring().write(interleaved.data(), block);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void PlaybackController::followClock() {
    if (!playing_) {
        return;
    }
    const time::Rational& audioRate = sequence_->audioSampleRate();
    const std::int64_t clock = sink_ ? sink_->clockFrames() : 0;

    // Position is an exact rational function of elapsed audio, floored to
    // the frame the playhead is inside -- the same arithmetic the scheduler
    // uses, and for the same reason.
    const time::Rational elapsed = time::Rational{clock - anchorClock_, 1} / audioRate;
    const time::Rational advanced = elapsed * transport_.speed() * sequence_->frameRate();
    const auto position =
        anchorPosition_ + time::RationalTime{advanced.floorToInt(), sequence_->frameRate()};

    if (position.frames() >= sequence_->duration().frames() || position.frames() < 0) {
        stop();
        return;
    }
    emit moved(position);
}

}  // namespace zaro::app
