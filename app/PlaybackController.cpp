#include "PlaybackController.h"

#include <QDebug>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

namespace zaro::app {
namespace {

/// How far ahead of the device the pump tries to stay, in device buffers.
///
/// This is the whole tolerance for a slow block: a decode that takes longer
/// than what is buffered is a gap the device fills with silence, and a gap is a
/// click. Mixing reads from disk, so the hitch to survive is a disk one, not a
/// scheduling one -- six buffers is about 130ms at 48kHz, and it costs nothing
/// but the memory because a stop pauses the device rather than draining it.
/// The ring is sized larger still, in AudioSink, so the pump is never blocked
/// from topping up by a buffer that is exactly as big as the lead.
constexpr std::int64_t kLeadBuffers = 6;

}  // namespace

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
        publishAnchor();
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
    if (sink_) {
        // Whatever the last run left in the ring is audio from wherever the
        // playhead was then, and the device would play it before a sample of
        // this run was heard -- a fragment of the old position, and then every
        // frame after it a ring's worth behind the picture for as long as
        // playback lasts. The device is paused here, so this is the one moment
        // the buffer can be dropped safely.
        sink_->ring().reset();
    }
    anchorPosition_ = from;
    anchorClock_ = sink_ ? sink_->clockFrames() : 0;
    audioWritten_ = anchorClock_;
    publishAnchor();
    playing_ = true;
    emit playingChanged(true);

    if (sink_) {
        // Audio on its own thread, for the reason ADR-006 records: sharing
        // one with rendering starves the device the moment a frame is slow.
        //
        // The device is *not* started here. It is started by the pump, once
        // the pump has something to give it -- see pumpAudio.
        audioRunning_.store(true, std::memory_order_relaxed);
        audioThread_ = std::thread{[this] { pumpAudio(); }};
    }
    clockTimer_->start();
}

void PlaybackController::publishAnchor() {
    const time::Rational& audioRate = sequence_->audioSampleRate();
    // Position first, then the clock it is anchored to. The pump reads them in
    // the other order, so the worst it can see is an anchor one write old --
    // never one value from before a re-anchor paired with one from after.
    anchorAudioFrames_.store(anchorPosition_.rescaledTo(audioRate).frames(),
                             std::memory_order_relaxed);
    atUnitySpeed_.store(transport_.speed() == time::Rational::fromInt(1),
                        std::memory_order_relaxed);
    anchorClockFrames_.store(anchorClock_, std::memory_order_release);
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
    constexpr std::int64_t kBlockFrames = 1024;

    // One buffer for the whole run. Mixing a block used to allocate one, which
    // put a call into the allocator between the device and the only thread that
    // can feed it -- and an allocator that stalls under memory pressure stalls
    // it exactly when a stall is audible.
    std::vector<float> interleaved(
        static_cast<std::size_t>(kBlockFrames) * static_cast<std::size_t>(kChannels), 0.0F);

    // Silence handed to the device is not a pause. The ring counts it as time
    // delivered because it really did pass, so every sample the device takes
    // before the first block is mixed is timeline time nobody hears -- and
    // since the clock is the device, picture stays that far ahead of sound for
    // the rest of the run. Starting the device only once there is something in
    // the ring is what keeps the first frame of playback in sync with the first
    // sound of it.
    bool deviceStarted = false;
    bool warnedUnreadable = false;

    while (audioRunning_.load(std::memory_order_relaxed)) {
        const std::int64_t lead =
            static_cast<std::int64_t>(sink_->deviceBufferFrames()) * kLeadBuffers;
        const std::int64_t target = sink_->clockFrames() + lead;

        while (audioWritten_ < target && audioRunning_.load(std::memory_order_relaxed)) {
            const std::int64_t block = std::min(kBlockFrames, target - audioWritten_);
            if (sink_->ring().availableToWrite() < block) {
                break;
            }
            const auto used = static_cast<std::size_t>(block * kChannels);
            std::fill_n(interleaved.begin(), used, 0.0F);

            // Read the clock anchor before the position it anchors, mirroring
            // the order publishAnchor writes them.
            const std::int64_t anchorClock = anchorClockFrames_.load(std::memory_order_acquire);
            const std::int64_t anchorFrames = anchorAudioFrames_.load(std::memory_order_relaxed);

            // Only at 1x: shuttling needs pitch handling to sound like
            // anything, so it runs silent while the clock keeps time.
            if (atUnitySpeed_.load(std::memory_order_relaxed)) {
                const time::RationalTime from{anchorFrames + (audioWritten_ - anchorClock),
                                              audioRate};
                if (auto mixed = mixer.mix(*sequence_, from, block, kChannels)) {
                    // Silence because a clip would not read is not the same as
                    // silence because the timeline is quiet, and the two are
                    // identical in the buffer. Said once per run, from the
                    // thread that found it, so a project whose media has moved
                    // explains itself instead of just playing nothing.
                    if (mixer.lastUnreadableClipCount() > 0 && !warnedUnreadable) {
                        warnedUnreadable = true;
                        qWarning().noquote()
                            << "playback: a clip's audio could not be read, so it is silent --"
                            << QString::fromStdString(mixer.lastReadError());
                    }
                    {
                        // Copied under a lock for the UI to read. The realtime
                        // path is the device callback draining the ring, and it
                        // never comes near this lock.
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

        if (!deviceStarted) {
            // Whatever that first pass managed, good or bad. If the mix cannot
            // keep up we would rather play badly than not start: the clock is
            // the device, so a device that never runs is a playhead that never
            // moves.
            deviceStarted = true;
            sink_->start();
        }

        // Sleep for a fraction of what is actually buffered rather than a fixed
        // couple of milliseconds. Healthy playback then wakes this thread a few
        // times a buffer instead of five hundred times a second, and a ring
        // that is draining shortens the nap on its own.
        const std::int64_t bufferedFrames =
            std::max<std::int64_t>(0, audioWritten_ - sink_->clockFrames());
        const std::int64_t bufferedMillis =
            bufferedFrames * 1000 / std::max<std::int64_t>(1, audioRate.roundToInt());
        const std::int64_t nap = std::clamp<std::int64_t>(bufferedMillis / 4, 1, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(nap));
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
