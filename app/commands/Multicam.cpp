// Lining up the angles of a multicam clip.

#include "Multicam.h"

#include <cstdint>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/edit/Sync.h"

namespace zaro::app::commands {

Result<AngleSyncReport> syncAngles(const Context& context, bool byEar) {
    const model::Clip* clip = context.selectedClip();
    if (clip == nullptr || !clip->isMulticam()) {
        return Error{ErrorCode::InvalidData, "select a multicam clip to sync"};
    }
    if (byEar && context.media == nullptr) {
        return Error{ErrorCode::InvalidData, "there is no media open to listen to"};
    }

    auto synced = byEar ? edit::syncByAudio(context.project(), *clip, *context.media)
                        : edit::syncByTimecode(context.project(), *clip);
    if (!synced) {
        return synced.error();
    }

    AngleSyncReport report;
    report.total = static_cast<std::int32_t>(clip->angles.size());
    std::vector<std::pair<std::int32_t, time::RationalTime>> offsets;
    for (const edit::AngleSync& entry : *synced) {
        if (entry.offset.has_value()) {
            offsets.emplace_back(entry.angle, *entry.offset);
            continue;
        }
        const std::string& name = clip->angles[static_cast<std::size_t>(entry.angle)].name;
        report.skipped.push_back(name + ": " + entry.reason);
    }

    if (!offsets.empty()) {
        auto built =
            edit::makeSetAngleOffsets(context.project(), context.target(), context.clip, offsets);
        if (!built) {
            return built.error();
        }
        context.commands().execute(context.project(), std::move(*built));
    }
    report.synced = static_cast<std::int32_t>(offsets.size());
    return report;
}

}  // namespace zaro::app::commands
