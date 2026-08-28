// Plays a project, for real, against a real audio device.
//
// There is no window: picture is composited and handed to the scheduler, and
// what gets reported is the timing. That is deliberate -- the hard part of
// playback is the clock, the queue and the drop policy, and all of it is
// measurable without anything to look at. The window is the app shell's job.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "Cli.h"
#include "zaro/core/io/ProjectIo.h"
#include "zaro/core/playback/PlaybackScheduler.h"
#include "zaro/core/playback/Transport.h"
#include "zaro/core/render/AudioGraph.h"
#include "zaro/core/render/RenderGraph.h"
#include "zaro/platform/ffmpeg/FFmpegMedia.h"
#include "zaro/platform/ffmpeg/FFmpegRender.h"
#include "zaro/platform/qrhi/GpuRenderGraph.h"
#include "zaro/platform/sdl/AudioSink.h"

namespace {

void printUsage() {
    std::puts("usage: zaro-play <project.zaro> [options]");
    std::puts("");
    std::puts("  --start <frame>   where to begin (default 0)");
    std::puts("  --seconds <n>     how long to play (default: to the end)");
    std::puts("  --speed <r>       playback speed, e.g. 1, 2, 1/2 (default 1)");
    std::puts("  --no-audio        run the clock without opening a device");
    std::puts("  --queue <n>       frames of render lookahead (default 8)");
    std::puts("  --cpu             composite on the CPU instead of the GPU");
    std::puts("  --version         print the version and exit");
}

}  // namespace

int main(int argc, char** argv) {
    if (zaro::tools::handledVersion(argc, argv, "zaro-play")) {
        return 0;
    }
    if (argc < 2) {
        printUsage();
        return 2;
    }

    const std::string projectPath = argv[1];
    std::int64_t startFrame = 0;
    double seconds = 0.0;
    zaro::time::Rational speed = zaro::time::Rational::fromInt(1);
    bool useAudio = true;
    std::size_t queueDepth = 8;
    bool forceCpu = false;

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--start" && i + 1 < argc) {
            startFrame = std::atoll(argv[++i]);
        } else if (arg == "--seconds" && i + 1 < argc) {
            seconds = std::atof(argv[++i]);
        } else if (arg == "--speed" && i + 1 < argc) {
            if (const auto parsed = zaro::time::Rational::parse(argv[++i])) {
                speed = *parsed;
            }
        } else if (arg == "--no-audio") {
            useAudio = false;
        } else if (arg == "--cpu") {
            forceCpu = true;
        } else if (arg == "--queue" && i + 1 < argc) {
            queueDepth = static_cast<std::size_t>(std::atoll(argv[++i]));
        } else {
            std::fprintf(stderr, "zaro-play: unknown option '%s'\n", arg.c_str());
            return 2;
        }
    }

    zaro::platform::ffmpeg::installLogHandler(false);

    auto loaded = zaro::io::loadProject(projectPath);
    if (!loaded) {
        std::fprintf(stderr, "zaro-play: %s\n", loaded.error().toString().c_str());
        return 1;
    }
    const zaro::model::Project& project = loaded->project;
    const zaro::model::Sequence* sequence = project.findSequence(project.activeSequence());
    if (sequence == nullptr) {
        std::fprintf(stderr, "zaro-play: this project has no active sequence\n");
        return 1;
    }

    const zaro::time::Rational& rate = sequence->frameRate();
    const zaro::time::Rational& audioRate = sequence->audioSampleRate();
    constexpr std::int32_t kChannels = 2;

    auto sourceOpened = zaro::platform::ffmpeg::ProjectMediaSource::open(project);
    if (!sourceOpened) {
        std::fprintf(stderr, "zaro-play: %s\n", sourceOpened.error().toString().c_str());
        return 1;
    }
    zaro::render::RenderGraph video{**sourceOpened};
    zaro::render::AudioGraph audio{**sourceOpened};

    // The GPU path uploads the decoder's planes and converts on the way
    // through; the CPU path converts into an 8MB float buffer first. At 1080p
    // that is 314 fps against 37, which is the difference between playing and
    // not. See docs/adr/0007.
    std::unique_ptr<zaro::platform::qrhi::GpuCompositor> compositor;
    std::unique_ptr<zaro::platform::qrhi::GpuRenderGraph> gpuVideo;
    if (!forceCpu) {
        if (auto created = zaro::platform::qrhi::GpuCompositor::create()) {
            compositor = std::move(*created);
            gpuVideo =
                std::make_unique<zaro::platform::qrhi::GpuRenderGraph>(*compositor, **sourceOpened);
        } else {
            std::fprintf(stderr, "zaro-play: no GPU (%s); compositing on the CPU\n",
                         created.error().message().c_str());
        }
    }

    std::unique_ptr<zaro::platform::sdl::AudioSink> sink;
    if (useAudio) {
        auto opened = zaro::platform::sdl::AudioSink::open(audioRate, kChannels);
        if (!opened) {
            std::fprintf(stderr, "zaro-play: %s\n", opened.error().toString().c_str());
            return 1;
        }
        sink = std::move(*opened);
    }

    zaro::playback::PlaybackScheduler::Config config;
    config.frameRate = rate;
    config.audioRate = audioRate;
    config.queueCapacity = queueDepth;
    config.duration = sequence->duration();
    zaro::playback::PlaybackScheduler scheduler{config};

    const auto startAt = zaro::time::RationalTime{startFrame, rate};
    scheduler.start(startAt, speed, 0);

    // Audio is generated from the start position forward. Shuttle speeds other
    // than 1x would need pitch handling to sound like anything, so they run
    // silent for now -- the clock still advances, because the device is still
    // being fed.
    const bool playAudio = useAudio && speed == zaro::time::Rational::fromInt(1);
    std::int64_t audioWritten = 0;
    zaro::render::RgbaImage frame;

    // Mix ahead of where the device is reading. Called before the device is
    // started as well as inside the loop: starting a device with an empty ring
    // guarantees a burst of underruns before the producer has said anything,
    // which is audible as a click at the top of every playback.
    const auto fillAudio = [&](std::int64_t clock) {
        if (!sink) {
            return;
        }
        const std::int64_t target = clock + sink->deviceBufferFrames() * 3;
        while (audioWritten < target) {
            const std::int64_t block = std::min<std::int64_t>(1024, target - audioWritten);
            if (sink->ring().availableToWrite() < block) {
                return;
            }
            std::vector<float> interleaved(static_cast<std::size_t>(block * kChannels), 0.0F);
            if (playAudio) {
                const auto position = startAt.rescaledTo(audioRate) +
                                      zaro::time::RationalTime{audioWritten, audioRate};
                if (auto mixed = audio.mix(*sequence, position, block, kChannels)) {
                    for (std::int64_t i = 0; i < mixed->sampleCount(); ++i) {
                        for (std::int32_t c = 0; c < kChannels; ++c) {
                            interleaved[static_cast<std::size_t>(i * kChannels + c)] =
                                mixed->channel(c)[i];
                        }
                    }
                }
            }
            audioWritten += sink->ring().write(interleaved.data(), block);
        }
    };

    // One place that renders a frame, whichever path is in use. The GPU path
    // still reads back here because the scheduler's queue holds CPU images;
    // removing that readback needs a GPU-resident frame in the queue, which is
    // what a preview window will want and is the next step.
    const auto renderOne = [&](const zaro::time::RationalTime& at) -> bool {
        if (gpuVideo) {
            return gpuVideo->compositeInto(*sequence, at, frame).ok();
        }
        return video.compositeInto(*sequence, at, frame).ok();
    };

    // Prime the ring, and render the first frames, before anything starts
    // consuming. Playback should begin already in its stride.
    fillAudio(0);
    while (const auto target = scheduler.nextRenderTarget()) {
        if (!renderOne(*target)) {
            break;
        }
        scheduler.submit(*target, frame.clone());
    }

    // Audio gets its own thread, and this is not an optimisation.
    //
    // Sharing a thread with the compositor means a frame that takes too long
    // starves the audio device, which is exactly backwards: the policy is that
    // video is dropped and audio never is. Measured on a 1080p59.94 timeline
    // the single-threaded version produced 200,000 samples of underrun -- four
    // seconds of silence -- purely because rendering was in the way.
    std::atomic<bool> running{true};
    std::thread audioThread;
    if (sink) {
        audioThread = std::thread{[&] {
            while (running.load(std::memory_order_relaxed)) {
                fillAudio(sink->clockFrames());
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }};
    }

    if (sink) {
        sink->start();
    }
    const auto began = std::chrono::steady_clock::now();
    const std::int64_t limitSamples =
        seconds > 0.0 ? static_cast<std::int64_t>(seconds * audioRate.toDouble()) : -1;

    std::int64_t lastReport = 0;

    while (true) {
        const std::int64_t clock =
            sink ? sink->clockFrames()
                 : static_cast<std::int64_t>(
                       std::chrono::duration<double>(std::chrono::steady_clock::now() - began)
                           .count() *
                       audioRate.toDouble());

        if (limitSamples > 0 && clock >= limitSamples) {
            break;
        }

        // Render ahead, then show whatever is due.
        while (const auto target = scheduler.nextRenderTarget()) {
            if (!renderOne(*target)) {
                break;
            }
            scheduler.submit(*target, frame.clone());
        }

        const auto result = scheduler.present(clock);
        if (result.action == zaro::playback::PresentAction::Idle) {
            break;
        }
        if (!scheduler.nextRenderTarget().has_value() && scheduler.queued() == 0 &&
            result.action != zaro::playback::PresentAction::Present) {
            break;  // ran off the end
        }

        const std::int64_t elapsedSeconds =
            static_cast<std::int64_t>(static_cast<double>(clock) / audioRate.toDouble());
        if (elapsedSeconds != lastReport) {
            lastReport = elapsedSeconds;
            std::printf("\r  %llds  frame %lld  presented %lld  dropped %lld  starved %lld",
                        static_cast<long long>(elapsedSeconds),
                        static_cast<long long>(scheduler.positionAt(clock).frames()),
                        static_cast<long long>(scheduler.stats().presented),
                        static_cast<long long>(scheduler.stats().dropped),
                        static_cast<long long>(scheduler.stats().starved));
            std::fflush(stdout);
        }

        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }

    running.store(false, std::memory_order_relaxed);
    if (audioThread.joinable()) {
        audioThread.join();
    }
    if (sink) {
        sink->pause();
    }
    const double wall =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();

    const auto& stats = scheduler.stats();
    std::printf("\n%s\n", projectPath.c_str());
    std::printf("  played %.2fs of wall clock at %s, compositing on the %s\n", wall,
                rate.toString().c_str(), gpuVideo ? compositor->backendName().c_str() : "CPU");
    std::printf("  presented %lld, dropped %lld, repeated %lld, starved %lld\n",
                static_cast<long long>(stats.presented), static_cast<long long>(stats.dropped),
                static_cast<long long>(stats.repeated), static_cast<long long>(stats.starved));
    std::printf("  worst picture offset from the clock: %lld frame(s)\n",
                static_cast<long long>(stats.worstOffsetFrames));
    if (sink) {
        std::printf("  audio underruns: %lld samples\n",
                    static_cast<long long>(sink->underrunFrames()));
    }
    // Two different failures, worth telling apart.
    //
    // An audio underrun means the clock itself was starved -- the one thing
    // that must never happen, because audio is what the audience hears and what
    // everything else is timed against. Picture lagging the clock means the
    // renderer could not keep up, which is a speed problem, not a sync problem:
    // the timeline still ran at the right rate, it just showed fewer frames.
    const bool clockHealthy = sink == nullptr || sink->underrunFrames() == 0;
    const bool rendererKeptUp = stats.worstOffsetFrames <= 1;

    if (!clockHealthy) {
        std::printf("  OUT OF SYNC: the audio clock was starved\n");
    } else if (!rendererKeptUp) {
        std::printf("  IN SYNC, but the renderer could not keep up: picture lagged by up to\n");
        std::printf("  %lld frames and %lld were dropped. Audio was never interrupted.\n",
                    static_cast<long long>(stats.worstOffsetFrames),
                    static_cast<long long>(stats.dropped));
    } else {
        std::printf("  in sync\n");
    }
    return clockHealthy && rendererKeptUp ? 0 : 1;
}
