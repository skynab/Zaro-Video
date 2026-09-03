// The window's chrome: the bars round the edges, and what they say.

#include "Bars.h"

#include <QSignalBlocker>

namespace zaro::app::chrome {

void refresh(const Bars& bars, const Status& status) {
    bars.projectLabel->setText(QString("%1 · %2%3")
                                   .arg(status.projectName,
                                        status.haveSequence ? status.sequenceName : QString{"—"},
                                        status.modified ? " •" : ""));
    bars.autosaveLabel->setText(status.modified ? "Unsaved changes" : "Saved");

    if (status.haveSequence) {
        bars.formatLabel->setText(QString("%1×%2 · %3 fps · Rec.709")
                                      .arg(status.width)
                                      .arg(status.height)
                                      .arg(status.frameRate, 0, 'g', 5));
        bars.timelineLabel->setText(
            QString("%1 · %2").arg(status.sequenceName, status.durationTimecode));
        bars.viewerLabel->setText(QString("%1 — %2").arg(status.projectName, status.sequenceName));
    }
    bars.qualityLabel->setText(status.comparing ? "Compare · CPU" : "Full · GPU");

    bars.statusLeft->setText(
        QString("%1 tool · %2 workspace").arg(status.toolName, status.workspace));
    if (status.inDeliver) {
        // In Deliver the interesting middle fact is the queue, not the bin, and
        // the tool bar's left label is the range rather than the format.
        bars.statusMiddle->setText(status.deliverStatus);
        bars.formatLabel->setText(status.deliverRange);
        bars.renderButton->setText(status.rendering ? "Stop render" : "Start render");
    } else {
        bars.statusMiddle->setText(QString("%1 %2 · %3")
                                       .arg(status.binItems)
                                       .arg(status.binItems == 1 ? "item" : "items",
                                            status.modified ? "edited" : "clean"));
    }
    // The missing device takes the right-hand slot while it is missing. What
    // normally sits there is the platform and the Qt version, which nobody is
    // reading while wondering why the playhead will not move.
    bars.statusRight->setText(
        status.audioDeviceMissing
            ? QString("no audio device — the playhead is driven by one, so playback cannot run")
            : QString("%1 · Qt %2").arg(status.platformLabel, QT_VERSION_STR));

    // Whether snapping is on is said by the magnet being lit, and saying it
    // again in words beside the button was the same fact twice.
    bars.snapButton->setChecked(status.snapEnabled);
    for (std::size_t i = 0; i < bars.toolButtons.size(); ++i) {
        bars.toolButtons[i]->setChecked(i == status.toolIndex);
    }
    if (!bars.zoomSlider->isSliderDown()) {
        const QSignalBlocker blocker{bars.zoomSlider};
        bars.zoomSlider->setValue(static_cast<int>(status.zoomFraction * 1000.0));
    }
    if (bars.rowHeightSlider != nullptr && !bars.rowHeightSlider->isSliderDown()) {
        const QSignalBlocker blocker{bars.rowHeightSlider};
        bars.rowHeightSlider->setValue(static_cast<int>(status.trackHeightFraction * 1000.0));
    }
}

}  // namespace zaro::app::chrome
