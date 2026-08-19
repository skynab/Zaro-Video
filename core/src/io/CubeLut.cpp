#include "zaro/core/io/CubeLut.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace zaro::io {
namespace {

/// Everything before a `#`, trimmed. The format's only comment marker.
std::string strip(const std::string& line) {
    const std::size_t hash = line.find('#');
    std::string out = hash == std::string::npos ? line : line.substr(0, hash);
    const std::size_t first = out.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = out.find_last_not_of(" \t\r\n");
    return out.substr(first, last - first + 1);
}

bool readTriple(std::istringstream& stream, std::array<float, 3>& out) {
    return static_cast<bool>(stream >> out[0] >> out[1] >> out[2]);
}

}  // namespace

Result<CubeLut> CubeLut::parse(const std::string& text) {
    CubeLut lut;
    std::int32_t declaredSize = 0;
    bool haveShape = false;

    std::istringstream lines{text};
    std::string raw;
    while (std::getline(lines, raw)) {
        const std::string line = strip(raw);
        if (line.empty()) {
            continue;
        }

        std::istringstream stream{line};
        std::string keyword;
        stream >> keyword;

        if (keyword == "TITLE") {
            // The rest of the line, quotes stripped. Not load-bearing, but a
            // LUT with a name is easier to recognise than a path.
            std::string rest;
            std::getline(stream, rest);
            const std::size_t open = rest.find('"');
            const std::size_t close = rest.rfind('"');
            lut.title_ = open != std::string::npos && close > open
                             ? rest.substr(open + 1, close - open - 1)
                             : strip(rest);
            continue;
        }
        if (keyword == "LUT_3D_SIZE" || keyword == "LUT_1D_SIZE") {
            if (haveShape) {
                return Error{ErrorCode::InvalidData, "the file declares two LUT sizes"};
            }
            if (!(stream >> declaredSize) || declaredSize < 2) {
                return Error{ErrorCode::InvalidData, "the LUT size is not a number above one"};
            }
            lut.shape_ = keyword == "LUT_3D_SIZE" ? Shape::ThreeD : Shape::OneD;
            lut.size_ = declaredSize;
            haveShape = true;
            const std::size_t count = lut.shape_ == Shape::ThreeD
                                          ? static_cast<std::size_t>(declaredSize) *
                                                static_cast<std::size_t>(declaredSize) *
                                                static_cast<std::size_t>(declaredSize)
                                          : static_cast<std::size_t>(declaredSize);
            lut.entries_.reserve(count * 3);
            continue;
        }
        if (keyword == "DOMAIN_MIN") {
            if (!readTriple(stream, lut.domainMin_)) {
                return Error{ErrorCode::InvalidData, "DOMAIN_MIN needs three numbers"};
            }
            continue;
        }
        if (keyword == "DOMAIN_MAX") {
            if (!readTriple(stream, lut.domainMax_)) {
                return Error{ErrorCode::InvalidData, "DOMAIN_MAX needs three numbers"};
            }
            continue;
        }

        // Anything else has to be a data row, which means the size must
        // already have been declared: a row read before the size has nowhere
        // to go.
        std::array<float, 3> triple{};
        std::istringstream row{line};
        if (!readTriple(row, triple)) {
            return Error{ErrorCode::InvalidData, "unrecognised line: " + line};
        }
        if (!haveShape) {
            return Error{ErrorCode::InvalidData, "the file has data before its LUT size"};
        }
        lut.entries_.push_back(triple[0]);
        lut.entries_.push_back(triple[1]);
        lut.entries_.push_back(triple[2]);
    }

    if (!haveShape) {
        return Error{ErrorCode::InvalidData, "the file declares no LUT size"};
    }
    const std::size_t expected = lut.shape_ == Shape::ThreeD
                                     ? static_cast<std::size_t>(lut.size_) *
                                           static_cast<std::size_t>(lut.size_) *
                                           static_cast<std::size_t>(lut.size_) * 3
                                     : static_cast<std::size_t>(lut.size_) * 3;
    if (lut.entries_.size() != expected) {
        return Error{ErrorCode::InvalidData,
                     "the file has " + std::to_string(lut.entries_.size() / 3) + " entries but " +
                         "declares " + std::to_string(expected / 3)};
    }
    for (int channel = 0; channel < 3; ++channel) {
        if (!(lut.domainMax_[static_cast<std::size_t>(channel)] >
              lut.domainMin_[static_cast<std::size_t>(channel)])) {
            return Error{ErrorCode::InvalidData, "the domain is empty or inverted"};
        }
    }
    return lut;
}

Result<CubeLut> CubeLut::load(const std::string& path) {
    std::ifstream file{path};
    if (!file) {
        return Error{ErrorCode::NotFound, "cannot open " + path};
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return parse(buffer.str());
}

std::array<float, 3> CubeLut::entryAt(std::int32_t index) const {
    const auto at = static_cast<std::size_t>(index) * 3;
    if (at + 2 >= entries_.size()) {
        return {0.0F, 0.0F, 0.0F};
    }
    return {entries_[at], entries_[at + 1], entries_[at + 2]};
}

void CubeLut::apply(float& r, float& g, float& b) const {
    if (size_ < 2) {
        return;
    }

    // Normalised into the LUT's own domain, and clamped to it: a LUT says
    // nothing about values it was not built for, and extrapolating a look
    // produces colours nobody chose.
    const auto normalise = [this](float value, std::size_t channel) {
        const float low = domainMin_[channel];
        const float high = domainMax_[channel];
        return std::clamp((value - low) / (high - low), 0.0F, 1.0F);
    };
    const float nr = normalise(r, 0);
    const float ng = normalise(g, 1);
    const float nb = normalise(b, 2);
    const auto last = static_cast<float>(size_ - 1);

    if (shape_ == Shape::OneD) {
        const auto sample = [&](float value, std::size_t channel) {
            const float scaled = value * last;
            const auto low = static_cast<std::int32_t>(scaled);
            const std::int32_t high = std::min(low + 1, size_ - 1);
            const float fraction = scaled - static_cast<float>(low);
            const std::array<float, 3> a = entryAt(low);
            const std::array<float, 3> c = entryAt(high);
            return a[channel] + ((c[channel] - a[channel]) * fraction);
        };
        r = sample(nr, 0);
        g = sample(ng, 1);
        b = sample(nb, 2);
        return;
    }

    // Trilinear. The red index moves fastest, which is what the format
    // specifies; getting that backwards swaps the red and blue axes of every
    // look, which reads as a colour problem rather than an indexing one.
    const float fr = nr * last;
    const float fg = ng * last;
    const float fb = nb * last;
    const auto r0 = static_cast<std::int32_t>(fr);
    const auto g0 = static_cast<std::int32_t>(fg);
    const auto b0 = static_cast<std::int32_t>(fb);
    const std::int32_t r1 = std::min(r0 + 1, size_ - 1);
    const std::int32_t g1 = std::min(g0 + 1, size_ - 1);
    const std::int32_t b1 = std::min(b0 + 1, size_ - 1);
    const float dr = fr - static_cast<float>(r0);
    const float dg = fg - static_cast<float>(g0);
    const float db = fb - static_cast<float>(b0);

    const auto at = [this](std::int32_t ri, std::int32_t gi, std::int32_t bi) {
        return entryAt(ri + (gi * size_) + (bi * size_ * size_));
    };
    std::array<float, 3> out{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const float c000 = at(r0, g0, b0)[channel];
        const float c100 = at(r1, g0, b0)[channel];
        const float c010 = at(r0, g1, b0)[channel];
        const float c110 = at(r1, g1, b0)[channel];
        const float c001 = at(r0, g0, b1)[channel];
        const float c101 = at(r1, g0, b1)[channel];
        const float c011 = at(r0, g1, b1)[channel];
        const float c111 = at(r1, g1, b1)[channel];

        const float c00 = c000 + ((c100 - c000) * dr);
        const float c10 = c010 + ((c110 - c010) * dr);
        const float c01 = c001 + ((c101 - c001) * dr);
        const float c11 = c011 + ((c111 - c011) * dr);
        const float c0 = c00 + ((c10 - c00) * dg);
        const float c1 = c01 + ((c11 - c01) * dg);
        out[channel] = c0 + ((c1 - c0) * db);
    }
    r = out[0];
    g = out[1];
    b = out[2];
}

}  // namespace zaro::io
