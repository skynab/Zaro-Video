#include "zaro/core/render/BakeLut.h"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "zaro/core/render/ColorPipeline.h"
#include "zaro/core/render/CurveTable.h"
#include "zaro/core/render/Grade.h"
#include "zaro/core/render/LutTable.h"

namespace zaro::render {

std::string LutOmissions::describe() const {
    std::vector<std::string> parts;
    if (mask) {
        parts.emplace_back("its mask");
    }
    if (vignette) {
        parts.emplace_back("its vignette");
    }
    if (keyer) {
        parts.emplace_back("its key");
    }
    if (effects) {
        parts.emplace_back("its effects");
    }
    if (aboveWhite) {
        parts.emplace_back("everything it lifts above white");
    }
    if (parts.empty()) {
        return {};
    }
    std::string out = "A look file cannot carry ";
    for (std::size_t i = 0; i < parts.size(); ++i) {
        out += parts[i];
        if (i + 2 < parts.size()) {
            out += ", ";
        } else if (i + 2 == parts.size()) {
            out += " or ";
        }
    }
    return out + ".";
}

Result<std::string> bakeCube(const model::Clip& clip, media::TransferFunction transfer,
                             std::int32_t size, const std::string& title, LutOmissions* omissions) {
    // 33 is the size everything reads and most things write. Below about 17 the
    // interpolation error starts showing on a gradient; above 65 the file is
    // megabytes for a difference nobody can see.
    if (size < 2 || size > 129) {
        return Error{ErrorCode::InvalidData, "a cube of that size is not usable"};
    }

    LutOmissions left;
    left.mask = clip.mask.isSet();
    left.vignette = clip.vignette.isSet();
    left.keyer = clip.keyer.isSet();
    left.effects = model::anyActive(clip.effects);

    const GradeConstants grade = gradeConstantsFor(clip.color, clip.wheels);
    CurveTableCache curves;
    const CurveTable& table = curves.tableFor(clip.id.value(), clip.curves, transfer);
    const SecondaryConstants secondary = secondaryConstantsFor(clip.secondary, transfer);
    LutCache looks;
    const LutTable* look = clip.lut.isSet() ? looks.tableFor(clip.lut.path, transfer) : nullptr;

    std::ostringstream out;
    out << "# Written by Zaro\n";
    if (!title.empty()) {
        out << "TITLE \"" << title << "\"\n";
    }
    out << "LUT_3D_SIZE " << size << "\n";
    out << "DOMAIN_MIN 0.0 0.0 0.0\n";
    out << "DOMAIN_MAX 1.0 1.0 1.0\n";
    out << std::fixed;
    out.precision(6);

    const auto axis = [size](std::int32_t index) {
        return static_cast<float>(index) / static_cast<float>(size - 1);
    };

    // Red fastest, then green, then blue: the order the format specifies and
    // the one CubeLut reads.
    for (std::int32_t bi = 0; bi < size; ++bi) {
        for (std::int32_t gi = 0; gi < size; ++gi) {
            for (std::int32_t ri = 0; ri < size; ++ri) {
                // In through the delivery curve, graded in linear light, out
                // through the same curve -- so the file describes the look as
                // it was judged on the scopes.
                float r = toLinearScalar(axis(ri), transfer);
                float g = toLinearScalar(axis(gi), transfer);
                float b = toLinearScalar(axis(bi), transfer);
                gradePixel(grade, r, g, b, &table, &secondary, look,
                           static_cast<float>(clip.lut.amount));

                float encodedR = fromLinearScalar(r, transfer);
                float encodedG = fromLinearScalar(g, transfer);
                float encodedB = fromLinearScalar(b, transfer);
                if (encodedR > 1.0F || encodedG > 1.0F || encodedB > 1.0F) {
                    // The grade puts light where a 0..1 cube has no room. Said
                    // rather than silently clipped, because the difference
                    // between this file and the shot is exactly what somebody
                    // needs to know before shipping it.
                    left.aboveWhite = true;
                }
                out << std::clamp(encodedR, 0.0F, 1.0F) << " " << std::clamp(encodedG, 0.0F, 1.0F)
                    << " " << std::clamp(encodedB, 0.0F, 1.0F) << "\n";
            }
        }
    }

    if (omissions != nullptr) {
        *omissions = left;
    }
    return out.str();
}

}  // namespace zaro::render
