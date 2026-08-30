#include "ThumbnailCache.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "zaro/core/media/VideoFrame.h"
#include "zaro/core/render/ColorPipeline.h"
#include "zaro/core/render/RgbaImage.h"
#include "zaro/platform/ffmpeg/FFmpegMedia.h"

namespace zaro::app {
namespace {

/// How many frames to hold. A filmstrip across a wide window is on the order of
/// forty cells; this is enough for several screenfuls either side of where the
/// view is, which is what makes scrolling back feel free.
constexpr std::size_t kCacheLimit = 480;

/// How many decodes to keep queued. Beyond this the oldest request is dropped:
/// it was wanted for a repaint that has already happened, and if it is still on
/// screen the next repaint will ask again.
constexpr std::size_t kQueueLimit = 48;

}  // namespace

ThumbnailCache::ThumbnailCache(QObject* parent) : QObject{parent} {
    worker_ = std::thread{[this] { run(); }};
}

ThumbnailCache::~ThumbnailCache() {
    {
        // Under the mutex, not just atomically. The worker evaluates the wait
        // predicate while holding it; setting the flag outside leaves a window
        // where the notify lands between that evaluation and the sleep, is
        // lost, and the join below never returns.
        const std::lock_guard lock{mutex_};
        stopping_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

QImage ThumbnailCache::lookup(const std::string& path, const time::RationalTime& at, int height) {
    if (path.empty() || height <= 0) {
        return {};
    }
    const Key key{path, at.frames(), at.rate().num(), at.rate().den(), height};

    std::unique_lock lock{mutex_};
    if (const auto found = cache_.find(key); found != cache_.end()) {
        // Touch it: this is the "recently used" half of the eviction policy.
        recent_.splice(recent_.begin(), recent_, found->second.second);
        return found->second.first;
    }
    if (inFlight_.contains(key)) {
        return {};
    }
    inFlight_.emplace(key, true);
    queue_.push_back(Request{key, at});
    if (queue_.size() > kQueueLimit) {
        inFlight_.erase(queue_.front().key);
        queue_.pop_front();
    }
    lock.unlock();
    wake_.notify_one();
    return {};
}

void ThumbnailCache::dropPending() {
    const std::lock_guard lock{mutex_};
    for (const Request& request : queue_) {
        inFlight_.erase(request.key);
    }
    queue_.clear();
}

void ThumbnailCache::clear() {
    const std::lock_guard lock{mutex_};
    queue_.clear();
    inFlight_.clear();
    cache_.clear();
    recent_.clear();
}

void ThumbnailCache::store(const Key& key, QImage image) {
    const std::lock_guard lock{mutex_};
    inFlight_.erase(key);
    // A failed decode is cached as a null image rather than dropped. Otherwise
    // media that cannot be read is re-opened and re-failed on every repaint,
    // which is the most expensive way to draw nothing.
    recent_.push_front(key);
    cache_.insert_or_assign(key, std::pair{std::move(image), recent_.begin()});
    while (recent_.size() > kCacheLimit) {
        cache_.erase(recent_.back());
        recent_.pop_back();
    }
}

void ThumbnailCache::run() {
    // One decoder, kept open between requests. Opening a file costs a seek and
    // a header parse, and a filmstrip asks for forty frames from the same file
    // in a row -- reopening for each would dominate the decode itself.
    std::string openPath;
    std::unique_ptr<media::VideoDecoder> decoder;

    while (!stopping_) {
        Request request;
        {
            std::unique_lock lock{mutex_};
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) {
                return;
            }
            // Newest first: what was asked for most recently is what is on
            // screen now, and the older entries may already have scrolled away.
            request = std::move(queue_.back());
            queue_.pop_back();
        }

        if (request.key.path != openPath || decoder == nullptr) {
            auto opened = platform::ffmpeg::openVideoDecoder(request.key.path);
            if (!opened) {
                store(request.key, QImage{});
                decoder.reset();
                openPath.clear();
                emit ready();
                continue;
            }
            decoder = std::move(*opened);
            openPath = request.key.path;
        }

        auto frame = decoder->frameAtTime(request.at);
        if (!frame) {
            // The decoder may be left mid-stream by a failed seek, so it is
            // dropped rather than reused: one unreadable frame should not make
            // every frame after it unreadable too.
            store(request.key, QImage{});
            decoder.reset();
            openPath.clear();
            emit ready();
            continue;
        }

        render::RgbaImage linear;
        if (!render::toLinear(*frame, linear) || !linear.isValid()) {
            store(request.key, QImage{});
            emit ready();
            continue;
        }
        const int wide = linear.width();
        const int tall = linear.height();
        const int stride = wide * 3;
        std::vector<std::uint8_t> rgb(static_cast<std::size_t>(stride) *
                                      static_cast<std::size_t>(tall));
        if (!render::toDisplayRgb24(linear, rgb.data(), stride)) {
            store(request.key, QImage{});
            emit ready();
            continue;
        }

        // Copied on the way into the QImage, because the vector is gone at the
        // end of this iteration, and scaled down straight away so the cache
        // holds thumbnails rather than frames.
        const QImage full{rgb.data(), wide, tall, stride, QImage::Format_RGB888};
        store(request.key, full.scaledToHeight(request.key.height, Qt::SmoothTransformation)
                               .convertToFormat(QImage::Format_RGB32));
        emit ready();
    }
}

}  // namespace zaro::app
