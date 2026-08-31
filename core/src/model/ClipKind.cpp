#include "zaro/core/model/ClipKind.h"

namespace zaro::model {

const char* toString(ClipKind kind) noexcept {
    switch (kind) {
        case ClipKind::VideoMedia:
            return "video";
        case ClipKind::AudioMedia:
            return "audio";
        case ClipKind::Shape:
            return "shape";
        case ClipKind::Text:
            return "text";
        case ClipKind::Adjustment:
            return "adjustment";
        case ClipKind::Multicam:
            return "multicam";
        case ClipKind::Nested:
            return "nested";
    }
    return "video";
}

std::span<const ClipKind> allClipKinds() noexcept {
    static constexpr ClipKind kAll[] = {
        ClipKind::VideoMedia, ClipKind::AudioMedia, ClipKind::Shape,  ClipKind::Text,
        ClipKind::Adjustment, ClipKind::Multicam,   ClipKind::Nested,
    };
    return kAll;
}

ClipKind clipKindOf(const Clip& clip, TrackKind track) noexcept {
    if (track == TrackKind::Audio) {
        return ClipKind::AudioMedia;
    }
    if (clip.adjustment) {
        return ClipKind::Adjustment;
    }
    switch (clip.graphic.kind) {
        case GraphicKind::Text:
            return ClipKind::Text;
        case GraphicKind::Rectangle:
        case GraphicKind::Ellipse:
            return ClipKind::Shape;
        case GraphicKind::None:
            break;
    }
    if (clip.nested.isValid()) {
        return ClipKind::Nested;
    }
    if (clip.isMulticam()) {
        return ClipKind::Multicam;
    }
    return ClipKind::VideoMedia;
}

bool hasPicture(ClipKind kind) noexcept {
    return kind != ClipKind::AudioMedia && kind != ClipKind::Adjustment;
}

bool readsMedia(ClipKind kind) noexcept {
    switch (kind) {
        case ClipKind::VideoMedia:
        case ClipKind::AudioMedia:
        case ClipKind::Multicam:
            return true;
        case ClipKind::Shape:
        case ClipKind::Text:
        case ClipKind::Adjustment:
        case ClipKind::Nested:
            return false;
    }
    return false;
}

}  // namespace zaro::model
