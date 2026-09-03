// The menus that ask a question, and nothing else.

#include "Choices.h"

#include <QAction>
#include <QCursor>
#include <QMenu>
#include <cstdint>
#include <map>

namespace zaro::app::chrome {

MulticamChoice multicamMenu(bool isMulticam, bool hasMedia) {
    QMenu menu;
    QAction* byTimecode = menu.addAction("Sync angles by timecode");
    QAction* byAudio = menu.addAction("Sync angles by audio");
    byTimecode->setEnabled(isMulticam);
    byAudio->setEnabled(isMulticam && hasMedia);
    if (!isMulticam) {
        menu.addSeparator();
        menu.addAction("Select a multicam clip first")->setEnabled(false);
    }

    QAction* chosen = menu.exec(QCursor::pos());
    if (chosen == nullptr || !isMulticam) {
        return MulticamChoice::None;
    }
    return chosen == byAudio ? MulticamChoice::ByAudio : MulticamChoice::ByTimecode;
}

RenderChoice renderMenu(std::int64_t visibleFrames, std::size_t cachedFrames,
                        std::size_t cachedBytes) {
    QMenu menu;
    QAction* render =
        menu.addAction(QString("Render the visible range (%1 frames)").arg(visibleFrames));
    render->setEnabled(visibleFrames > 0);
    QAction* clear = menu.addAction("Clear the render cache");
    menu.addSeparator();
    menu.addAction(
            QString("%1 frames cached, %2 MB").arg(cachedFrames).arg(cachedBytes / (1024 * 1024)))
        ->setEnabled(false);

    QAction* chosen = menu.exec(QCursor::pos());
    if (chosen == clear) {
        return RenderChoice::ClearCache;
    }
    if (chosen == render) {
        return RenderChoice::RenderVisible;
    }
    return RenderChoice::None;
}

CaptionChoice captionsMenu(std::size_t captionCount, bool burnedIn) {
    QMenu menu;
    QAction* importAction = menu.addAction("Import subtitles…");
    QAction* exportAction = menu.addAction("Export subtitles…");
    menu.addSeparator();
    QAction* burnAction = menu.addAction("Burn in");
    burnAction->setCheckable(true);
    burnAction->setChecked(burnedIn);
    burnAction->setEnabled(captionCount > 0);
    exportAction->setEnabled(captionCount > 0);
    menu.addSeparator();
    menu.addAction(QString("%1 captions").arg(captionCount))->setEnabled(false);

    QAction* chosen = menu.exec(QCursor::pos());
    if (chosen == importAction) {
        return CaptionChoice::Import;
    }
    if (chosen == exportAction) {
        return CaptionChoice::Export;
    }
    if (chosen == burnAction) {
        return CaptionChoice::ToggleBurnIn;
    }
    return CaptionChoice::None;
}

bool loudnessMenu(double integratedLufs, double samplePeakDbfs, double targetLufs) {
    const double delta = targetLufs - integratedLufs;
    QMenu menu;
    menu.addAction(QString("Integrated: %1 LUFS").arg(integratedLufs, 0, 'f', 1))
        ->setEnabled(false);
    menu.addAction(QString("Sample peak: %1 dBFS").arg(samplePeakDbfs, 0, 'f', 1))
        ->setEnabled(false);
    menu.addSeparator();
    QAction* normalise = menu.addAction(QString("Normalise to %1 LUFS (%2%3 dB)")
                                            .arg(targetLufs, 0, 'f', 1)
                                            .arg(delta >= 0.0 ? "+" : "")
                                            .arg(delta, 0, 'f', 1));
    return menu.exec(QCursor::pos()) == normalise;
}

DeliveryChoice deliveryMenu(media::TransferFunction currentTransfer, double currentKnee) {
    QMenu menu;
    menu.addAction("Delivered through")->setEnabled(false);
    std::map<QAction*, media::TransferFunction> curves;
    for (const media::TransferFunction transfer : media::allTransferFunctions()) {
        if (transfer == media::TransferFunction::Unknown) {
            continue;  // no formula, so nothing could encode through it
        }
        QAction* action = menu.addAction(QString::fromUtf8(media::toString(transfer)));
        action->setCheckable(true);
        action->setChecked(currentTransfer == transfer);
        curves.emplace(action, transfer);
    }

    menu.addSeparator();
    menu.addAction("Highlights")->setEnabled(false);
    struct Knee {
        const char* name;
        double value;
    };
    static constexpr Knee kKnees[] = {
        {"clip (as delivered before)", 1.0},
        {"roll off gently", 0.9},
        {"roll off", 0.8},
        {"roll off hard", 0.65},
    };
    std::map<QAction*, double> knees;
    for (const Knee& knee : kKnees) {
        QAction* action = menu.addAction(QString::fromUtf8(knee.name));
        action->setCheckable(true);
        action->setChecked(currentKnee == knee.value);
        knees.emplace(action, knee.value);
    }

    QAction* chosen = menu.exec(QCursor::pos());
    DeliveryChoice choice;
    if (chosen == nullptr) {
        return choice;
    }
    if (const auto curve = curves.find(chosen); curve != curves.end()) {
        choice.transfer = curve->second;
    } else if (const auto knee = knees.find(chosen); knee != knees.end()) {
        choice.highlightKnee = knee->second;
    }
    return choice;
}

FrameSizeChoice frameSizeMenu(std::int32_t currentWidth, std::int32_t currentHeight,
                              std::int32_t sourceWidth, std::int32_t sourceHeight) {
    QMenu menu;
    menu.addAction(QString("Frame size — now %1 × %2").arg(currentWidth).arg(currentHeight))
        ->setEnabled(false);
    menu.addSeparator();

    std::map<QAction*, std::pair<std::int32_t, std::int32_t>> sizes;
    const auto offer = [&](const QString& label, std::int32_t width, std::int32_t height) {
        if (width <= 0 || height <= 0) {
            return;
        }
        QAction* action =
            menu.addAction(QString("%1  (%2 × %3)").arg(label).arg(width).arg(height));
        action->setCheckable(true);
        action->setChecked(currentWidth == width && currentHeight == height);
        sizes.emplace(action, std::pair{width, height});
    };

    // The footage first. A sequence that does not match what is on it is the
    // reason somebody opened this menu, and matching it is nearly always the
    // answer -- the export does not scale, so anything else crops or pads.
    if (sourceWidth > 0 && sourceHeight > 0) {
        offer("Match the footage", sourceWidth - (sourceWidth % 2),
              sourceHeight - (sourceHeight % 2));
        menu.addSeparator();
    }

    offer("HD", 1920, 1080);
    offer("HD ready", 1280, 720);
    offer("4K UHD", 3840, 2160);
    menu.addSeparator();
    offer("Vertical HD", 1080, 1920);
    offer("Vertical, small", 720, 1280);
    offer("Square", 1080, 1080);

    menu.addSeparator();
    QAction* custom = menu.addAction("Custom…");

    QAction* chosen = menu.exec(QCursor::pos());
    FrameSizeChoice choice;
    if (chosen == nullptr) {
        return choice;
    }
    if (chosen == custom) {
        choice.chosen = true;
        choice.custom = true;
        return choice;
    }
    if (const auto found = sizes.find(chosen); found != sizes.end()) {
        choice.chosen = true;
        choice.width = found->second.first;
        choice.height = found->second.second;
    }
    return choice;
}

namespace {

/// "23.976", "25", "29.97" -- five significant figures is enough to tell every
/// rate in `time::rates` apart and short enough to sit in a menu label.
QString rateLabel(const time::Rational& rate) {
    return QString::number(rate.toDouble(), 'g', 5);
}

}  // namespace

FrameRateChoice frameRateMenu(const time::Rational& currentRate, const time::Rational& sourceRate,
                              bool hasClips) {
    QMenu menu;
    menu.addAction(QString("Frame rate — now %1 fps").arg(rateLabel(currentRate)))
        ->setEnabled(false);
    menu.addSeparator();

    if (hasClips) {
        // The door is locked, but there is a sign on it: without this the menu
        // item either did not exist -- what shipped before -- or opened a
        // picker that failed on the click, which explains nothing at the
        // moment somebody is looking for an explanation.
        menu.addAction("Every clip's position is measured in frames at this rate, so changing")
            ->setEnabled(false);
        menu.addAction("it now would silently retime the whole sequence. Move everything")
            ->setEnabled(false);
        menu.addAction("off this sequence first, or start a new one at the rate you want.")
            ->setEnabled(false);
        menu.exec(QCursor::pos());
        return FrameRateChoice{};
    }

    std::map<QAction*, time::Rational> rates;
    const auto offer = [&](const QString& label, const time::Rational& rate) {
        QAction* action = menu.addAction(QString("%1  (%2 fps)").arg(label, rateLabel(rate)));
        action->setCheckable(true);
        action->setChecked(currentRate == rate);
        rates.emplace(action, rate);
    };

    // The footage first, for the reason frameSizeMenu puts the footage first:
    // matching what is about to be cut is nearly always the answer somebody
    // opened this menu for.
    if (sourceRate.isPositive()) {
        offer("Match the footage", sourceRate);
        menu.addSeparator();
    }

    offer("Film, 24", time::rates::fps24);
    offer("NTSC film, 23.976", time::rates::fps23_976);
    offer("PAL, 25", time::rates::fps25);
    offer("NTSC, 29.97", time::rates::fps29_97);
    offer("30", time::rates::fps30);
    menu.addSeparator();
    offer("PAL high frame rate, 50", time::rates::fps50);
    offer("NTSC high frame rate, 59.94", time::rates::fps59_94);
    offer("60", time::rates::fps60);

    QAction* chosen = menu.exec(QCursor::pos());
    FrameRateChoice choice;
    if (chosen == nullptr) {
        return choice;
    }
    if (const auto found = rates.find(chosen); found != rates.end()) {
        choice.chosen = true;
        choice.rate = found->second;
    }
    return choice;
}

ProxyChoice proxyMenu(const std::vector<ProxyEntry>& entries, bool usingProxies) {
    std::size_t proxied = 0;
    for (const ProxyEntry& entry : entries) {
        proxied += entry.hasProxy ? 1 : 0;
    }

    QMenu menu;
    QAction* toggle = menu.addAction("Use proxies");
    toggle->setCheckable(true);
    toggle->setChecked(usingProxies);
    toggle->setEnabled(proxied > 0);
    menu.addSeparator();
    menu.addAction(QString("%1 of %2 have proxies").arg(proxied).arg(entries.size()))
        ->setEnabled(false);
    menu.addSeparator();

    std::map<QAction*, model::MediaRefId> attach;
    std::map<QAction*, model::MediaRefId> build;
    for (const ProxyEntry& entry : entries) {
        if (entry.hasProxy) {
            continue;
        }
        QAction* action = menu.addAction(QString("Make a proxy for %1").arg(entry.name));
        build.emplace(action, entry.media);
    }
    menu.addSeparator();
    for (const ProxyEntry& entry : entries) {
        if (!entry.hasProxy) {
            continue;
        }
        QAction* action = menu.addAction(QString("Attach a proxy to %1…").arg(entry.name));
        attach.emplace(action, entry.media);
    }

    QAction* chosen = menu.exec(QCursor::pos());
    ProxyChoice choice;
    if (chosen == nullptr) {
        return choice;
    }
    if (chosen == toggle) {
        choice.kind = ProxyChoice::Kind::ToggleUsingProxies;
        return choice;
    }
    if (const auto making = build.find(chosen); making != build.end()) {
        choice.kind = ProxyChoice::Kind::Build;
        choice.media = making->second;
        return choice;
    }
    if (const auto found = attach.find(chosen); found != attach.end()) {
        choice.kind = ProxyChoice::Kind::Attach;
        choice.media = found->second;
    }
    return choice;
}

}  // namespace zaro::app::chrome
