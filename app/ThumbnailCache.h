#pragma once

#include <QImage>
#include <QObject>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "zaro/core/time/RationalTime.h"

namespace zaro::app {

/// Frames from source media, small, for drawing filmstrips on timeline clips.
///
/// Built the way waveforms are: the expensive part happens on a worker thread
/// and the answer is handed back when it exists, because the alternative --
/// decoding inside `paintEvent` -- is a repaint that takes a second and a half
/// and an application that stops while you scroll.
///
/// Lookups never block and never decode. A miss returns a null image and queues
/// the work; when the frame arrives, `ready` is emitted and whoever asked can
/// paint again and get it. That makes a filmstrip fill in over a moment or two
/// rather than holding the timeline hostage until every cell is ready.
///
/// The cache is bounded and least-recently-used, so panning along a long
/// sequence costs a fixed amount of memory rather than accumulating every frame
/// it has ever shown.
class ThumbnailCache : public QObject {
    Q_OBJECT

public:
    explicit ThumbnailCache(QObject* parent = nullptr);
    ~ThumbnailCache() override;

    ThumbnailCache(const ThumbnailCache&) = delete;
    ThumbnailCache& operator=(const ThumbnailCache&) = delete;

    /// The frame at `at` in the file at `path`, `height` logical pixels tall.
    ///
    /// Null on a miss, with the decode queued. `at` is in the media's own
    /// timebase; the caller is expected to have quantised it, because a
    /// thumbnail per pixel of scroll is a cache that never hits.
    ///
    /// Addressed by path rather than by media id, so that the answer is about
    /// the file actually read. A media reference resolves to different files at
    /// different times -- its proxy when proxies are on, its own path when they
    /// are not, a new path after a relink -- and keying on the reference would
    /// hand back the original's frames the moment somebody switched to proxies.
    [[nodiscard]] QImage lookup(const std::string& path, const time::RationalTime& at, int height);

    /// Give up on everything queued but not yet decoded.
    ///
    /// Called when the view changes enough that what was wanted a moment ago is
    /// no longer on screen -- a zoom, or a jump to a different part of the cut.
    /// What is already decoded stays: it cost something, and it may well be
    /// wanted again.
    void dropPending();

    /// Forget everything. For closing a project, where the media itself is
    /// about to stop existing.
    void clear();

signals:
    /// At least one thumbnail arrived since the last repaint.
    void ready();

private:
    struct Key {
        std::string path;
        std::int64_t frame{0};
        /// The timebase the frame number is counted in. Two files at the same
        /// frame number are at different times unless this matches too.
        std::int64_t rateNum{0};
        std::int64_t rateDen{0};
        int height{0};

        friend auto operator<=>(const Key&, const Key&) = default;
    };

    struct Request {
        Key key;
        time::RationalTime at;
    };

    void run();
    /// Store a decoded frame and evict whatever has gone longest unused.
    void store(const Key& key, QImage image);

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    /// The decoded frames, and the order they were last asked for. The list
    /// holds the keys; the map points into it, so a hit is a splice rather than
    /// a search.
    std::map<Key, std::pair<QImage, std::list<Key>::iterator>> cache_;
    std::list<Key> recent_;
    std::deque<Request> queue_;
    /// Keys that are queued or being decoded, so the same frame is not asked
    /// for once per repaint while it is still being worked on.
    std::map<Key, bool> inFlight_;
    std::atomic<bool> stopping_{false};
    std::thread worker_;
};

}  // namespace zaro::app
