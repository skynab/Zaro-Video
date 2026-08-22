#include "zaro/core/model/Effect.h"

#include <cstring>

namespace zaro::model {

const char* toString(EffectKind kind) noexcept {
    switch (kind) {
        case EffectKind::Blur:
            return "blur";
        case EffectKind::Sharpen:
            return "sharpen";
        case EffectKind::Distort:
            return "distort";
    }
    return "";
}

std::span<const EffectKind> allEffectKinds() noexcept {
    static constexpr EffectKind kAll[] = {EffectKind::Blur, EffectKind::Sharpen,
                                          EffectKind::Distort};
    return kAll;
}

bool effectKindFromString(const char* name, EffectKind& out) noexcept {
    if (name == nullptr) {
        return false;
    }
    for (const EffectKind candidate : allEffectKinds()) {
        if (std::strcmp(name, toString(candidate)) == 0) {
            out = candidate;
            return true;
        }
    }
    return false;
}

const char* toString(EffectParam param) noexcept {
    switch (param) {
        case EffectParam::Radius:
            return "radius";
        case EffectParam::Amount:
            return "amount";
        case EffectParam::Curvature:
            return "curvature";
        case EffectParam::Zoom:
            return "zoom";
    }
    return "";
}

bool effectParamFromString(const char* name, EffectParam& out) noexcept {
    if (name == nullptr) {
        return false;
    }
    for (const EffectParam candidate :
         {EffectParam::Radius, EffectParam::Amount, EffectParam::Curvature, EffectParam::Zoom}) {
        if (std::strcmp(name, toString(candidate)) == 0) {
            out = candidate;
            return true;
        }
    }
    return false;
}

std::span<const EffectParamInfo> parametersOf(EffectKind kind) noexcept {
    // A radius of zero is the identity for both, so a freshly added effect
    // changes nothing until somebody moves a control. Adding an effect and
    // having the picture jump would make it impossible to tell what the effect
    // did from what it was set to.
    static constexpr EffectParamInfo kBlur[] = {
        {EffectParam::Radius, 0.0, 0.0, 200.0, 0.5},
    };
    static constexpr EffectParamInfo kSharpen[] = {
        {EffectParam::Radius, 1.0, 0.1, 50.0, 0.1},
        {EffectParam::Amount, 0.0, 0.0, 4.0, 0.05},
    };
    // A curvature of zero and a zoom of one are the identity, so a distortion
    // somebody has added and not yet set changes nothing.
    static constexpr EffectParamInfo kDistort[] = {
        {EffectParam::Curvature, 0.0, -1.0, 1.0, 0.01},
        {EffectParam::Zoom, 1.0, 0.25, 4.0, 0.01},
    };
    switch (kind) {
        case EffectKind::Blur:
            return kBlur;
        case EffectKind::Sharpen:
            return kSharpen;
        case EffectKind::Distort:
            return kDistort;
    }
    return {};
}

double Effect::value(EffectParam param) const {
    if (const auto found = values.find(param); found != values.end()) {
        return found->second;
    }
    for (const EffectParamInfo& info : parametersOf(kind)) {
        if (info.param == param) {
            return info.defaultValue;
        }
    }
    return 0.0;
}

const Curve* Effect::curve(EffectParam param) const {
    const auto found = animation.find(param);
    return found == animation.end() || found->second.empty() ? nullptr : &found->second;
}

bool Effect::isAnimated(EffectParam param) const {
    return curve(param) != nullptr;
}

double Effect::valueAt(EffectParam param, double seconds) const {
    const Curve* animated = curve(param);
    return animated == nullptr ? value(param) : animated->valueAtSeconds(seconds);
}

bool anyActive(const std::vector<Effect>& effects) {
    for (const Effect& effect : effects) {
        if (!effect.enabled) {
            continue;
        }
        if (!effect.animation.empty()) {
            return true;
        }
        switch (effect.kind) {
            case EffectKind::Blur:
                if (effect.value(EffectParam::Radius) > 0.0) {
                    return true;
                }
                break;
            case EffectKind::Sharpen:
                if (effect.value(EffectParam::Amount) > 0.0 &&
                    effect.value(EffectParam::Radius) > 0.0) {
                    return true;
                }
                break;
            case EffectKind::Distort:
                if (effect.value(EffectParam::Curvature) != 0.0 ||
                    effect.value(EffectParam::Zoom) != 1.0) {
                    return true;
                }
                break;
        }
    }
    return false;
}

}  // namespace zaro::model
