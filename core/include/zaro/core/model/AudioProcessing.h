#pragma once

#include <cstdint>

namespace zaro::model {

/// A track's equaliser.
///
/// Three sections, which is what a dialogue track actually needs: something to
/// take the rumble out, something to take the hiss off, and one bell to fix
/// whatever the room did. A parametric with eight bands is a different tool and
/// mostly a way to make a track worse more precisely.
struct AudioEq {
    bool enabled{false};
    /// Below this is removed. Zero switches the section off.
    double highPassHz{0.0};
    /// Above this is removed. Zero switches the section off.
    double lowPassHz{0.0};
    /// One bell: where, how much, and how wide.
    double peakHz{1000.0};
    double peakGainDb{0.0};
    double peakQ{1.0};

    friend bool operator==(const AudioEq&, const AudioEq&) = default;
};

/// A compressor, which is also the limiter when the ratio is high enough.
///
/// One control set rather than two boxes: a limiter is a compressor with a
/// ratio nobody argues with, and having both would mean explaining which one
/// runs first.
struct Compressor {
    bool enabled{false};
    double thresholdDb{-18.0};
    /// 1 is no compression. 20 and above is limiting.
    double ratio{3.0};
    double attackMs{10.0};
    double releaseMs{120.0};
    double makeupDb{0.0};

    friend bool operator==(const Compressor&, const Compressor&) = default;
};

}  // namespace zaro::model
