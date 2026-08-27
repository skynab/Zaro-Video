// The menus that ask a question, and nothing else.
//
// Each of these used to be a method on PreviewWindow that put a popup up,
// worked out what had been chosen, and then went and did it. Two jobs in one
// function, and the doing half is what tied the asking half to the window: a
// menu that offers "Sync angles by audio" needs to know nothing except whether
// that option should be greyed out.
//
// So they ask and return. Every one of these takes the handful of facts it
// needs to label and enable its items, and hands back what was picked. What
// happens next is the caller's, which is what lets the same question be asked
// from somewhere that is not this window -- and lets a test check that the
// option is greyed out when it should be, without a project or a GPU.
#pragma once

#include <QString>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "zaro/core/media/ColorInfo.h"
#include "zaro/core/model/Ids.h"

namespace zaro::app::chrome {

enum class MulticamChoice { None, ByTimecode, ByAudio };

/// Offer the two ways of lining angles up.
///
/// Both are offered even when neither can be used, greyed out with a line
/// saying why: a menu that is empty when nothing is selected looks broken,
/// and one that hides the thing somebody came for teaches nothing.
MulticamChoice multicamMenu(bool isMulticam, bool hasMedia);

enum class RenderChoice { None, RenderVisible, ClearCache };

RenderChoice renderMenu(std::int64_t visibleFrames, std::size_t cachedFrames,
                        std::size_t cachedBytes);

enum class CaptionChoice { None, Import, Export, ToggleBurnIn };

CaptionChoice captionsMenu(std::size_t captionCount, bool burnedIn);

/// Show what the programme measures, and offer to bring it to the target.
///
/// Returns true when normalising was asked for.
bool loudnessMenu(double integratedLufs, double samplePeakDbfs, double targetLufs);

/// What a delivery menu came back with: one of the two, or neither.
struct DeliveryChoice {
    std::optional<media::TransferFunction> transfer;
    std::optional<double> highlightKnee;
};

DeliveryChoice deliveryMenu(media::TransferFunction currentTransfer, double currentKnee);

/// What a proxy menu came back with.
struct ProxyChoice {
    enum class Kind { None, ToggleUsingProxies, Build, Attach };
    Kind kind{Kind::None};
    model::MediaRefId media;
};

/// One line per file: what it is called, and whether it already has a proxy.
struct ProxyEntry {
    model::MediaRefId media;
    QString name;
    bool hasProxy{false};
};

ProxyChoice proxyMenu(const std::vector<ProxyEntry>& entries, bool usingProxies);

}  // namespace zaro::app::chrome
