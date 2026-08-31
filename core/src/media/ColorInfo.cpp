#include "zaro/core/media/ColorInfo.h"

#include <cstdint>
#include <cstring>

namespace zaro::media {

ColorInfo ColorInfo::resolved(std::int32_t width, std::int32_t height) const {
    ColorInfo out = *this;

    // The dividing line the whole industry uses: anything taller than 576 lines
    // is HD-era and therefore BT.709; below that it is SD and therefore BT.601,
    // split by line count into its 525- and 625-line variants.
    const bool isStandardDefinition = height > 0 && height <= 576 && width <= 1024;
    const bool is525Line = height <= 480;

    if (out.primaries == ColorPrimaries::Unknown) {
        out.primaries = isStandardDefinition
                            ? (is525Line ? ColorPrimaries::BT601_525 : ColorPrimaries::BT601_625)
                            : ColorPrimaries::BT709;
    }
    if (out.transfer == TransferFunction::Unknown) {
        out.transfer = isStandardDefinition ? TransferFunction::SMPTE170M : TransferFunction::BT709;
    }
    if (out.matrix == ColorMatrix::Unknown) {
        out.matrix = isStandardDefinition ? ColorMatrix::BT601 : ColorMatrix::BT709;
    }
    if (out.range == ColorRange::Unknown) {
        // Y'CbCr from a camera or a delivery codec is limited range unless it
        // says otherwise. Assuming full range here is the more damaging error:
        // it crushes blacks and clips highlights rather than merely flattening.
        out.range = ColorRange::Limited;
    }
    return out;
}

const char* toString(ColorPrimaries v) noexcept {
    switch (v) {
        case ColorPrimaries::Unknown:
            return "unknown";
        case ColorPrimaries::BT709:
            return "bt709";
        case ColorPrimaries::BT601_525:
            return "bt601-525";
        case ColorPrimaries::BT601_625:
            return "bt601-625";
        case ColorPrimaries::BT2020:
            return "bt2020";
        case ColorPrimaries::DisplayP3:
            return "display-p3";
    }
    return "unknown";
}

std::span<const ColorPrimaries> allColorPrimaries() noexcept {
    static constexpr ColorPrimaries kAll[] = {
        ColorPrimaries::Unknown,   ColorPrimaries::BT709,  ColorPrimaries::BT601_525,
        ColorPrimaries::BT601_625, ColorPrimaries::BT2020, ColorPrimaries::DisplayP3,
    };
    return kAll;
}

bool colorPrimariesFromString(const char* name, ColorPrimaries& out) noexcept {
    if (name == nullptr) {
        return false;
    }
    for (const ColorPrimaries candidate : allColorPrimaries()) {
        if (std::strcmp(name, toString(candidate)) == 0) {
            out = candidate;
            return true;
        }
    }
    return false;
}

const char* toString(TransferFunction v) noexcept {
    switch (v) {
        case TransferFunction::Unknown:
            return "unknown";
        case TransferFunction::BT709:
            return "bt709";
        case TransferFunction::Gamma22:
            return "gamma22";
        case TransferFunction::Gamma28:
            return "gamma28";
        case TransferFunction::SMPTE170M:
            return "smpte170m";
        case TransferFunction::SRGB:
            return "srgb";
        case TransferFunction::Linear:
            return "linear";
        case TransferFunction::PQ:
            return "pq";
        case TransferFunction::HLG:
            return "hlg";
        case TransferFunction::SLog3:
            return "slog3";
        case TransferFunction::VLog:
            return "vlog";
        case TransferFunction::LogC3:
            return "logc3";
    }
    return "unknown";
}

std::span<const TransferFunction> allTransferFunctions() noexcept {
    static constexpr TransferFunction kAll[] = {
        TransferFunction::Unknown, TransferFunction::BT709,     TransferFunction::Gamma22,
        TransferFunction::Gamma28, TransferFunction::SMPTE170M, TransferFunction::SRGB,
        TransferFunction::Linear,  TransferFunction::PQ,        TransferFunction::HLG,
        TransferFunction::SLog3,   TransferFunction::VLog,      TransferFunction::LogC3};
    return kAll;
}

bool transferFunctionFromString(const char* name, TransferFunction& out) noexcept {
    if (name == nullptr) {
        return false;
    }
    for (const TransferFunction candidate : allTransferFunctions()) {
        if (std::strcmp(name, toString(candidate)) == 0) {
            out = candidate;
            return true;
        }
    }
    return false;
}

const char* toString(ColorMatrix v) noexcept {
    switch (v) {
        case ColorMatrix::Unknown:
            return "unknown";
        case ColorMatrix::BT709:
            return "bt709";
        case ColorMatrix::BT601:
            return "bt601";
        case ColorMatrix::BT2020NCL:
            return "bt2020ncl";
        case ColorMatrix::SMPTE240M:
            return "smpte240m";
        case ColorMatrix::Identity:
            return "identity";
    }
    return "unknown";
}

const char* toString(ColorRange v) noexcept {
    switch (v) {
        case ColorRange::Unknown:
            return "unknown";
        case ColorRange::Limited:
            return "limited";
        case ColorRange::Full:
            return "full";
    }
    return "unknown";
}

std::string toString(const ColorInfo& v) {
    return std::string{toString(v.primaries)} + "/" + toString(v.transfer) + "/" +
           toString(v.matrix) + "/" + toString(v.range);
}

}  // namespace zaro::media
