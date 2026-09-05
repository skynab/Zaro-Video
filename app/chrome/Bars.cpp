// The window's chrome: the bars round the edges, and what they say.

#include "Bars.h"

#include <QPoint>
#include <QSignalBlocker>
#include <QVariant>
#include <vector>

#include "Choices.h"

namespace zaro::app::chrome {

namespace {

/// Fill the frame-size dropdown, and select what the sequence actually is.
///
/// Rebuilt only when the list or the selection would change. `refresh` runs on
/// every edit, and repopulating a combo box that often closes its popup the
/// moment somebody opens it -- which reads as a dropdown that refuses to stay
/// open rather than as a redraw.
void refreshFormatBox(QComboBox& box, const Status& status) {
    const QString signature = QString("%1x%2|%3x%4|%5")
                                  .arg(status.width)
                                  .arg(status.height)
                                  .arg(status.sourceWidth)
                                  .arg(status.sourceHeight)
                                  .arg(status.haveSequence ? 1 : 0);
    if (box.property("signature").toString() == signature) {
        return;
    }
    box.setProperty("signature", signature);

    const QSignalBlocker quiet{box};
    box.clear();
    if (!status.haveSequence) {
        box.addItem(QStringLiteral("—"));
        return;
    }

    const std::vector<FrameSizePreset> presets =
        frameSizePresets(status.sourceWidth, status.sourceHeight);
    bool matched = false;
    for (const FrameSizePreset& preset : presets) {
        if (preset.isCustom()) {
            continue;
        }
        // The size, and a word only where the size does not speak for itself.
        // "1920 × 1080" needs no gloss and the closed box has to stay narrow
        // enough to sit in a toolbar; "Match the footage" is the one entry
        // whose numbers do not say what it is for.
        const bool isMatch = preset.label == QStringLiteral("Match the footage");
        box.addItem(isMatch ? QString("%1 × %2  (footage)").arg(preset.width).arg(preset.height)
                            : QString("%1 × %2").arg(preset.width).arg(preset.height),
                    QVariant{QPoint{preset.width, preset.height}});
        if (preset.width == status.width && preset.height == status.height) {
            box.setCurrentIndex(box.count() - 1);
            matched = true;
        }
    }
    // A size nobody offered is still the size this sequence is, so it goes in
    // the list and is selected. Showing a preset instead would have the box
    // assert a resolution the project does not have.
    if (!matched) {
        box.insertItem(0, QString("%1 × %2").arg(status.width).arg(status.height),
                       QVariant{QPoint{status.width, status.height}});
        box.setCurrentIndex(0);
    }
    box.insertSeparator(box.count());
    box.addItem(QStringLiteral("Custom…"), QVariant{QPoint{0, 0}});
}

}  // namespace

void refresh(const Bars& bars, const Status& status) {
    bars.projectLabel->setText(QString("%1 · %2%3")
                                   .arg(status.projectName,
                                        status.haveSequence ? status.sequenceName : QString{"—"},
                                        status.modified ? " •" : ""));
    // Three states, not two. A project that has never been written is not
    // "Saved" however untouched it is, and saying so on a window somebody just
    // opened is the readout claiming work is safe that is not on disk at all.
    bars.autosaveLabel->setText(status.modified ? "Unsaved changes"
                                : status.onDisk ? "Saved"
                                                : "Not saved yet");

    if (status.haveSequence) {
        // Split in two so each half opens the thing that sets it: the size is
        // pressed to change the size, the rate to change the rate.
        // The chevron is doing real work: it is what tells somebody scanning
        // the bar that these two are openable at all.
        bars.rateButton->setText(QString("%1 fps · Rec.709 ⌄").arg(status.frameRate, 0, 'g', 5));
        bars.timelineLabel->setText(
            QString("%1 · %2").arg(status.sequenceName, status.durationTimecode));
        bars.viewerLabel->setText(QString("%1 — %2").arg(status.projectName, status.sequenceName));
    }
    bars.qualityLabel->setText(status.comparing ? "Compare · CPU" : "Full · GPU");

    bars.statusLeft->setText(
        QString("%1 tool · %2 workspace").arg(status.toolName, status.workspace));
    if (status.inDeliver) {
        // In Deliver the interesting middle fact is the queue, not the bin, and
        // the tool bar's left readout is the range rather than the format. A
        // range is not a setting, so it is shown plain and does not open a menu.
        bars.statusMiddle->setText(status.deliverStatus);
        // The dropdown steps aside: what belongs in this slot here is the
        // range being rendered, which is a fact rather than a setting.
        bars.formatBox->setVisible(false);
        bars.formatButton->setVisible(true);
        bars.formatButton->setText(status.deliverRange);
        bars.formatButton->setEnabled(false);
        bars.formatButton->setCursor(Qt::ArrowCursor);
        bars.rateButton->setVisible(false);
        bars.renderButton->setText(status.rendering ? "Stop render" : "Start render");
    } else {
        bars.formatButton->setVisible(false);
        bars.formatBox->setVisible(true);
        bars.formatBox->setEnabled(status.haveSequence);
        refreshFormatBox(*bars.formatBox, status);
        bars.rateButton->setVisible(status.haveSequence);
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
