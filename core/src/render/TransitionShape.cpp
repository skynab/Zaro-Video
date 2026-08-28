#include "zaro/core/render/TransitionShape.h"

#include <algorithm>
#include <cstdint>

namespace zaro::render {

TransitionShape transitionShapeFor(const model::Transition& transition, double progress,
                                   std::int32_t width, std::int32_t height) {
    TransitionShape out;
    const double t = std::clamp(progress, 0.0, 1.0);
    const auto frameWidth = static_cast<double>(width);
    const auto frameHeight = static_cast<double>(height);

    switch (transition.kind) {
        case model::TransitionKind::Wipe: {
            // A rectangle covering the part of the frame the edge has passed,
            // growing from the side the direction comes from. Hard-edged: a
            // feathered wipe is a different look and a different control, and
            // guessing a softness would be a decision nobody made.
            out.wipe.shape = model::MaskShape::Rectangle;
            out.wipe.width = frameWidth;
            out.wipe.height = frameHeight;
            switch (transition.direction) {
                case model::TransitionDirection::Right:
                    out.wipe.width = frameWidth * t;
                    out.wipe.centreX = ((frameWidth * t) - frameWidth) / 2.0;
                    break;
                case model::TransitionDirection::Left:
                    out.wipe.width = frameWidth * t;
                    out.wipe.centreX = (frameWidth - (frameWidth * t)) / 2.0;
                    break;
                case model::TransitionDirection::Down:
                    out.wipe.height = frameHeight * t;
                    out.wipe.centreY = ((frameHeight * t) - frameHeight) / 2.0;
                    break;
                case model::TransitionDirection::Up:
                    out.wipe.height = frameHeight * t;
                    out.wipe.centreY = (frameHeight - (frameHeight * t)) / 2.0;
                    break;
            }
            return out;
        }
        case model::TransitionKind::Slide: {
            // Off screen at the start, home at the end. The outgoing clip stays
            // where it is underneath: a slide that pushed both would be a
            // different transition, and one this cannot express without moving
            // a clip nobody asked to move.
            const double remaining = 1.0 - t;
            switch (transition.direction) {
                case model::TransitionDirection::Right:
                    out.offsetX = -frameWidth * remaining;
                    break;
                case model::TransitionDirection::Left:
                    out.offsetX = frameWidth * remaining;
                    break;
                case model::TransitionDirection::Down:
                    out.offsetY = -frameHeight * remaining;
                    break;
                case model::TransitionDirection::Up:
                    out.offsetY = frameHeight * remaining;
                    break;
            }
            return out;
        }
        case model::TransitionKind::CrossDissolve:
        default:
            // With premultiplied `over` and an opaque source this gives
            // out*(1-p) + in*p, which is what a dissolve is.
            out.opacity = t;
            return out;
    }
}

}  // namespace zaro::render
