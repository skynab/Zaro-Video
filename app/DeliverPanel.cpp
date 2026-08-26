#include "DeliverPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSplitter>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <filesystem>
#include <utility>

#include "zaro/core/time/Timecode.h"
#include "zaro/platform/qtext/QtTextRasterizer.h"

#include "Theme.h"

namespace zaro::app {

/// A switch, as the design draws them: a track with a knob that slides.
///
/// A checkbox says the same thing and is a third of the code, but a row of
/// eleven of them reads as a form to be filled in rather than as a set of
/// things that are on or off -- which is what these are.
class PillSwitch : public QAbstractButton {
public:
    explicit PillSwitch(const QString& label, QWidget* parent = nullptr) : QAbstractButton{parent} {
        setText(label);
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover, true);
        setFocusPolicy(Qt::StrongFocus);
    }

    [[nodiscard]] QSize sizeHint() const override {
        return {static_cast<int>(kTrack + kGap) + fontMetrics().horizontalAdvance(text()), 22};
    }

protected:
    void paintEvent(QPaintEvent* /*event*/) override {
        QPainter painter{this};
        painter.setRenderHint(QPainter::Antialiasing, true);

        const double top = (height() - kHeight) / 2.0;
        const QRectF track{0.0, top, kTrack, kHeight};
        painter.setPen(Qt::NoPen);
        painter.setBrush(isChecked() ? theme::mix(theme::surface(), theme::accent(), 0.62)
                                     : theme::mix(theme::surface(), theme::text(), 0.12));
        painter.drawRoundedRect(track, kHeight / 2.0, kHeight / 2.0);

        const double knob = isChecked() ? kTrack - kKnob - 2.0 : 2.0;
        painter.setBrush(isEnabled() ? theme::text() : theme::textAt(0.4));
        painter.drawEllipse(QRectF{knob, top + 2.0, kKnob, kKnob});

        painter.setPen(isEnabled() ? theme::textAt(0.68) : theme::textAt(0.35));
        painter.drawText(QRectF(kTrack + kGap, 0, width() - kTrack - kGap, height()),
                         Qt::AlignVCenter | Qt::AlignLeft, text());
        if (hasFocus()) {
            painter.setPen(QPen(theme::accent(), 1.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(track.adjusted(-1.5, -1.5, 1.5, 1.5), 10.0, 10.0);
        }
    }

private:
    static constexpr double kTrack = 30.0;
    static constexpr double kHeight = 17.0;
    static constexpr double kKnob = 13.0;
    static constexpr double kGap = 9.0;
};

/// A delivery preset: a container, what to encode with, and what to say about
/// it. Only fields the renderer honours -- there is no resolution here, because
/// changing it is not something this program can do yet and a menu that offered
/// it would be a promise the file breaks.
struct DeliverPanel::Preset {
    QString group;
    QString name;
    QString sub;
    QString description;
    QString extension;
    /// FFmpeg encoder names. Empty means "whatever the container defaults to",
    /// which is what every caller got before there was a choice.
    QString videoCodec;
    QString audioCodec;
    double quality{0.55};
    /// ProRes and PCM pick their own rate from the profile, so the quality
    /// control is meaningless for them and is disabled rather than ignored.
    bool takesBitRate{true};
    QString depth;
};

/// One queued render.
struct DeliverPanel::Job {
    enum class State { Queued, Rendering, Done, Failed, Stopped };

    QString name;
    QString spec;
    std::string path;
    /// A copy of the project as it was when the job was queued.
    ///
    /// A render takes minutes and nothing stops somebody carrying on editing
    /// while it runs. Rendering the live project would mean a file that is
    /// neither the cut they queued nor the one they have now, changing under
    /// the encoder halfway through.
    model::Project project;
    platform::ffmpeg::RenderRequest request;
    bool reveal{false};

    State state{State::Queued};
    double progress{0.0};
    std::int64_t framesDone{0};
    std::int64_t framesTotal{0};
    double elapsed{0.0};
    QString message;
};

namespace {

QLabel* sectionLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text.toUpper(), parent);
    label->setStyleSheet(QString("color:%1;font-size:10px;letter-spacing:1px")
                             .arg(theme::textAt(0.42).name(QColor::HexRgb)));
    return label;
}

QLabel* fieldLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setStyleSheet(
        QString("color:%1;font-size:11px").arg(theme::textAt(0.52).name(QColor::HexRgb)));
    return label;
}

/// A read-only fact, shown where the design has a control this program cannot
/// honour. It looks like a field and does not pretend to be one.
QLabel* factField(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setStyleSheet(QString("color:%1;font-size:12px;padding:5px 9px;border:1px solid %2;"
                                 "border-radius:6px;background:%3")
                             .arg(theme::textAt(0.62).name(QColor::HexRgb),
                                  theme::divider().name(QColor::HexRgb),
                                  theme::bg().name(QColor::HexRgb)));
    return label;
}

QString humanSize(double bytes) {
    if (bytes >= 1024.0 * 1024.0 * 1024.0) {
        return QString::number(bytes / (1024.0 * 1024.0 * 1024.0), 'f', 2) + " GB";
    }
    if (bytes >= 1024.0 * 1024.0) {
        return QString::number(bytes / (1024.0 * 1024.0), 'f', 0) + " MB";
    }
    // A ten-second draft of a small sequence really is a few hundred kilobytes,
    // and "0 MB" reads as an arithmetic bug rather than as a small file.
    return QString::number(bytes / 1024.0, 'f', 0) + " KB";
}

}  // namespace

DeliverPanel::DeliverPanel(QWidget* parent) : QWidget{parent} {
    presets_ = {
        Preset{"Web", "H.264 · MP4", "libx264 · AAC",
               "One-pass H.264 in MP4, the format everything plays. The quality "
               "control sets the picture's bit rate.",
               "mp4", "libx264", "aac", 0.55, true, "8-bit"},
        Preset{"Web", "H.265 · MP4", "libx265 · AAC",
               "Half the bit rate of H.264 for the same picture, and slower to "
               "encode. Needs an FFmpeg built with libx265.",
               "mp4", "libx265", "aac", 0.45, true, "10-bit"},
        Preset{"Web", "Draft · MP4", "libx264 · AAC · low rate",
               "Something to look at, quickly. Small and soft on purpose -- not "
               "a file to send anybody.",
               "mp4", "libx264", "aac", 0.12, true, "8-bit"},
        Preset{"Master", "ProRes · MOV", "prores_ks · PCM 24-bit",
               "Archive-grade intermediate with uncompressed sound. ProRes picks "
               "its own rate from the profile, so the quality control does "
               "nothing here.",
               "mov", "prores_ks", "pcm_s24le", 1.0, false, "10-bit"},
    };

    // A splitter rather than three fixed widths. The design is drawn at 1560
    // pixels, where 250 and 334 for the sides leave plenty in the middle; at
    // the width a window actually opens at they leave the settings column too
    // narrow to lay a form out in, and the form is the point of the screen.
    auto* columns = new QHBoxLayout(this);
    columns->setContentsMargins(0, 0, 0, 0);
    columns->setSpacing(0);
    auto* split = new QSplitter(Qt::Horizontal, this);
    split->setHandleWidth(1);
    split->setChildrenCollapsible(false);

    auto* presetColumn = new QVBoxLayout;
    presetColumn->setContentsMargins(0, 0, 0, 0);
    presetColumn->setSpacing(0);
    buildPresets(presetColumn);
    auto* presetHolder = new QWidget(split);
    presetHolder->setObjectName("deliver-side");
    presetHolder->setMinimumWidth(180);
    presetHolder->setMaximumWidth(280);
    presetHolder->setLayout(presetColumn);
    split->addWidget(presetHolder);

    auto* settingsColumn = new QVBoxLayout;
    settingsColumn->setContentsMargins(0, 0, 0, 0);
    settingsColumn->setSpacing(0);
    buildSettings(settingsColumn);
    auto* settingsHolder = new QWidget(split);
    settingsHolder->setMinimumWidth(360);
    settingsHolder->setLayout(settingsColumn);
    split->addWidget(settingsHolder);

    auto* queueColumn = new QVBoxLayout;
    queueColumn->setContentsMargins(0, 0, 0, 0);
    queueColumn->setSpacing(0);
    buildQueue(queueColumn);
    auto* queueHolder = new QWidget(split);
    queueHolder->setObjectName("deliver-side");
    queueHolder->setMinimumWidth(220);
    queueHolder->setMaximumWidth(380);
    queueHolder->setLayout(queueColumn);
    split->addWidget(queueHolder);

    // Stretch factors rather than explicit sizes: sizes handed to a splitter
    // before it has been laid out are normalised against a width it does not
    // have yet, and the middle column ended up wider than the window.
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setStretchFactor(2, 0);
    columns->addWidget(split);

    // The codec menus are filled by the container's own signal, and the
    // container starts on MP4 -- so picking MP4 later changes nothing and the
    // signal never fires. Once by hand, here, where every widget it touches
    // exists.
    emit container_->currentTextChanged(container_->currentText());
    applyPreset(0);
}

DeliverPanel::~DeliverPanel() {
    // Same argument as the preview window and the export dialog: a std::thread
    // still joinable at destruction calls std::terminate, and closing the
    // window mid-render is an ordinary thing to do.
    shuttingDown_.store(true, std::memory_order_relaxed);
    cancel_.store(true, std::memory_order_relaxed);
    if (worker_.joinable()) {
        worker_.join();
    }
}

void DeliverPanel::buildPresets(QVBoxLayout* into) {
    auto* header = new QWidget(this);
    header->setObjectName("deliver-header");
    header->setFixedHeight(32);
    auto* headerRow = new QHBoxLayout(header);
    headerRow->setContentsMargins(12, 0, 8, 0);
    auto* title = new QLabel("Presets", header);
    title->setStyleSheet("font-weight:600;font-size:12px");
    headerRow->addWidget(title);
    headerRow->addStretch(1);
    into->addWidget(header);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget(scroll);
    auto* list = new QVBoxLayout(content);
    list->setContentsMargins(8, 8, 8, 8);
    list->setSpacing(4);

    QString group;
    QListWidget* current = nullptr;
    for (std::size_t i = 0; i < presets_.size(); ++i) {
        const Preset& preset = presets_[i];
        if (preset.group != group) {
            group = preset.group;
            list->addWidget(sectionLabel(group, content));
            current = new QListWidget(content);
            current->setFrameShape(QFrame::NoFrame);
            current->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            current->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            current->setSelectionMode(QAbstractItemView::SingleSelection);
            current->setStyleSheet("background:transparent;border:none");
            presetLists_.push_back(current);
            list->addWidget(current);
            // One list per group, so the section headings can sit between them.
            // Picking in one clears the others, since together they are one
            // choice rather than three.
            connect(current, &QListWidget::itemSelectionChanged, this, [this, current] {
                if (current->currentItem() == nullptr || !current->hasFocus()) {
                    return;
                }
                applyPreset(current->currentItem()->data(Qt::UserRole).toInt());
            });
        }
        auto* item = new QListWidgetItem(preset.name + "\n" + preset.sub, current);
        item->setData(Qt::UserRole, static_cast<int>(i));
        item->setSizeHint(QSize(0, 40));
    }
    for (QListWidget* every : presetLists_) {
        every->setFixedHeight(every->count() * 40 + 4);
    }

    list->addStretch(1);
    scroll->setWidget(content);
    into->addWidget(scroll, 1);

    auto* rangeBox = new QWidget(this);
    auto* rangeLayout = new QVBoxLayout(rangeBox);
    rangeLayout->setContentsMargins(12, 10, 12, 10);
    rangeLayout->setSpacing(6);
    rangeLayout->addWidget(sectionLabel("Range", rangeBox));
    auto* rangeRow = new QHBoxLayout;
    rangeRow->setSpacing(3);
    // What the engine can actually be asked for: a start frame and a count.
    // "In to out" is not among them because a sequence has no in and out yet;
    // the playhead is what somebody has instead.
    const QStringList names{"Timeline", "To end", "Marker"};
    for (int i = 0; i < names.size(); ++i) {
        auto* button = new QPushButton(names.at(i), rangeBox);
        button->setCheckable(true);
        button->setChecked(i == 0);
        button->setProperty("flat", true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setFixedHeight(24);
        connect(button, &QPushButton::clicked, this, [this, i] {
            range_ = i;
            for (int other = 0; other < static_cast<int>(rangeButtons_.size()); ++other) {
                qobject_cast<QPushButton*>(rangeButtons_[static_cast<std::size_t>(other)])
                    ->setChecked(other == i);
            }
            updateDerived();
        });
        rangeButtons_.push_back(button);
        rangeRow->addWidget(button);
    }
    rangeLayout->addLayout(rangeRow);
    into->addWidget(rangeBox);
}

void DeliverPanel::buildSettings(QVBoxLayout* into) {
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget(scroll);
    auto* column = new QVBoxLayout(content);
    column->setContentsMargins(20, 16, 20, 16);
    column->setSpacing(16);

    // --- what this preset is ------------------------------------------------
    auto* head = new QVBoxLayout;
    head->setSpacing(8);
    presetName_ = new QLabel(content);
    presetName_->setStyleSheet("font-size:19px;font-weight:500");
    presetDescription_ = new QLabel(content);
    presetDescription_->setWordWrap(true);
    presetDescription_->setStyleSheet(
        QString("color:%1;font-size:12px").arg(theme::textAt(0.55).name(QColor::HexRgb)));
    head->addWidget(presetName_);
    head->addWidget(presetDescription_);

    auto* estimates = new QGridLayout;
    estimates->setHorizontalSpacing(10);
    const auto estimate = [&](int col, const QString& label, QLabel** slot, const QColor& colour) {
        estimates->addWidget(sectionLabel(label, content), 0, col);
        *slot = new QLabel("—", content);
        (*slot)->setStyleSheet(QString("font-size:14px;color:%1").arg(colour.name(QColor::HexRgb)));
        estimates->addWidget(*slot, 1, col);
    };
    estimate(0, "Est. size", &estimateSize_, theme::accent(200));
    estimate(1, "Bit rate", &estimateBitrate_, theme::textAt(0.78));
    estimate(2, "Duration", &estimateDuration_, theme::textAt(0.78));
    estimate(3, "Colour", &estimateColour_, theme::accent(300));
    head->addLayout(estimates);
    column->addLayout(head);

    auto* rule = new QFrame(content);
    rule->setFrameShape(QFrame::HLine);
    rule->setFixedHeight(1);
    rule->setStyleSheet(
        QString("background:%1;border:none").arg(theme::divider().name(QColor::HexRgb)));
    column->addWidget(rule);

    // --- destination --------------------------------------------------------
    column->addWidget(sectionLabel("Destination", content));
    auto* destination = new QGridLayout;
    destination->setHorizontalSpacing(10);
    destination->setVerticalSpacing(4);
    destination->addWidget(fieldLabel("File name", content), 0, 0);
    destination->addWidget(fieldLabel("Location", content), 0, 1);
    fileName_ = new QLineEdit("delivery", content);
    connect(fileName_, &QLineEdit::textChanged, this, [this] { updateDerived(); });
    destination->addWidget(fileName_, 1, 0);

    auto* locationRow = new QHBoxLayout;
    folder_ = new QLineEdit(QDir::homePath(), content);
    folder_->setReadOnly(true);
    auto* browse = new QPushButton("Browse", content);
    browse->setProperty("flat", true);
    connect(browse, &QPushButton::clicked, this, [this] { chooseFolder(); });
    locationRow->addWidget(folder_, 1);
    locationRow->addWidget(browse);
    destination->addLayout(locationRow, 1, 1);
    column->addLayout(destination);

    fullPath_ = new QLabel(content);
    fullPath_->setStyleSheet(
        QString("color:%1;font-size:11px").arg(theme::textAt(0.62).name(QColor::HexRgb)));
    fullPath_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // Wrapped, so a long path makes the column taller rather than wider: a
    // scroll area sizes itself to its widest child, and a delivery path is as
    // long as somebody's folder structure.
    fullPath_->setWordWrap(true);
    column->addWidget(fullPath_);

    // --- video --------------------------------------------------------------
    column->addWidget(sectionLabel("Video", content));
    auto* video = new QGridLayout;
    video->setHorizontalSpacing(10);
    video->setVerticalSpacing(4);
    video->addWidget(fieldLabel("Container", content), 0, 0);
    video->addWidget(fieldLabel("Codec", content), 0, 1);
    container_ = new QComboBox(content);
    container_->addItems({"MP4", "MOV"});
    codec_ = new QComboBox(content);
    // Menus size themselves to their longest entry by default, which sets a
    // floor under the whole column. The popup still shows the full text.
    for (QComboBox* box : {container_, codec_}) {
        box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        box->setMinimumContentsLength(8);
    }
    video->addWidget(container_, 1, 0);
    video->addWidget(codec_, 1, 1);

    video->addWidget(fieldLabel("Resolution", content), 2, 0);
    video->addWidget(fieldLabel("Frame rate", content), 2, 1);
    resolution_ = factField("—", content);
    frameRate_ = factField("—", content);
    video->addWidget(resolution_, 3, 0);
    video->addWidget(frameRate_, 3, 1);

    video->addWidget(fieldLabel("Bit depth", content), 4, 0);
    video->addWidget(fieldLabel("Sound", content), 4, 1);
    depth_ = factField("—", content);
    auto* audioSwitchHolder = new QWidget(content);
    auto* audioSwitchRow = new QHBoxLayout(audioSwitchHolder);
    audioSwitchRow->setContentsMargins(0, 4, 0, 0);
    audioOn_ = new PillSwitch("Include sound", audioSwitchHolder);
    audioOn_->setChecked(true);
    connect(audioOn_, &QAbstractButton::toggled, this, [this] { updateDerived(); });
    audioSwitchRow->addWidget(audioOn_);
    video->addWidget(depth_, 5, 0);
    video->addWidget(audioSwitchHolder, 5, 1);
    column->addLayout(video);

    // Resolution and frame rate are the sequence's, and this program has no
    // scaler or retimer on the export path -- so they are stated, not offered.
    auto* fixedNote = new QLabel(
        "Resolution and frame rate come from the sequence: export does not scale or retime.",
        content);
    fixedNote->setWordWrap(true);
    fixedNote->setStyleSheet(
        QString("color:%1;font-size:11px").arg(theme::textAt(0.42).name(QColor::HexRgb)));
    column->addWidget(fixedNote);

    // --- quality ------------------------------------------------------------
    auto* qualityRow = new QHBoxLayout;
    qualityName_ = fieldLabel("Quality", content);
    qualityRate_ = new QLabel(content);
    qualityRate_->setStyleSheet(
        QString("font-size:11px;color:%1").arg(theme::accent(300).name(QColor::HexRgb)));
    qualityRow->addWidget(qualityName_);
    qualityRow->addStretch(1);
    qualityRow->addWidget(qualityRate_);
    column->addLayout(qualityRow);

    quality_ = new QSlider(Qt::Horizontal, content);
    quality_->setRange(0, 1000);
    connect(quality_, &QSlider::valueChanged, this, [this] { updateDerived(); });
    column->addWidget(quality_);

    auto* ticks = new QHBoxLayout;
    for (const QString& name :
         {QString("Draft"), QString("Web"), QString("Broadcast"), QString("Master")}) {
        auto* tick = new QLabel(name, content);
        tick->setStyleSheet(
            QString("color:%1;font-size:10px").arg(theme::textAt(0.32).name(QColor::HexRgb)));
        ticks->addWidget(tick);
        if (name != "Master") {
            ticks->addStretch(1);
        }
    }
    column->addLayout(ticks);

    // --- audio --------------------------------------------------------------
    column->addWidget(sectionLabel("Audio", content));
    auto* audio = new QGridLayout;
    audio->setHorizontalSpacing(10);
    audio->setVerticalSpacing(4);
    audio->addWidget(fieldLabel("Codec", content), 0, 0);
    audio->addWidget(fieldLabel("Channels", content), 0, 1);
    audioCodec_ = new QComboBox(content);
    audioCodec_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    audioCodec_->setMinimumContentsLength(8);
    audioChannels_ = factField("Stereo", content);
    audio->addWidget(audioCodec_, 1, 0);
    audio->addWidget(audioChannels_, 1, 1);
    audio->addWidget(fieldLabel("Sample rate", content), 2, 0);
    audioRate_ = factField("—", content);
    audio->addWidget(audioRate_, 3, 0);
    column->addLayout(audio);

    loudness_ = new QLabel(content);
    loudness_->setWordWrap(true);
    loudness_->setStyleSheet(
        QString("color:%1;font-size:11px;padding:8px 10px;border-radius:6px;"
                "border:1px solid %2;background:%3")
            .arg(theme::textAt(0.70).name(QColor::HexRgb),
                 theme::mix(theme::bg(), theme::accent(), 0.22).name(QColor::HexRgb),
                 theme::mix(theme::bg(), theme::accent(), 0.08).name(QColor::HexRgb)));
    column->addWidget(loudness_);

    // --- advanced -----------------------------------------------------------
    column->addWidget(sectionLabel("Advanced", content));
    // One per row rather than the design's two columns: a switch's label is a
    // sentence, and two sentences side by side set a minimum width this column
    // does not have at the size a window opens at.
    auto* advanced = new QVBoxLayout;
    advanced->setSpacing(7);
    smartRender_ = new PillSwitch("Smart render — copy where nothing changed", content);
    smartRender_->setChecked(true);
    smartRender_->setToolTip(
        "Where the export is a piece of one file with nothing done to it, copy its packets "
        "instead of decoding and re-encoding them.");
    useGpu_ = new PillSwitch("Composite on the GPU", content);
    useGpu_->setChecked(true);
    revealWhenDone_ = new PillSwitch("Show the file when it finishes", content);
    advanced->addWidget(smartRender_);
    advanced->addWidget(useGpu_);
    advanced->addWidget(revealWhenDone_);
    column->addLayout(advanced);
    column->addStretch(1);

    connect(container_, &QComboBox::currentTextChanged, this, [this] {
        // The codecs a container will take are not the same list, so the menu
        // is rebuilt rather than left offering something that cannot be muxed.
        const QString wanted = container_->currentText();
        codec_->clear();
        if (wanted == "MOV") {
            codec_->addItems({"ProRes (prores_ks)", "H.264 (libx264)"});
            audioCodec_->clear();
            audioCodec_->addItems({"PCM 24-bit (pcm_s24le)", "PCM 16-bit (pcm_s16le)", "AAC"});
        } else {
            codec_->addItems({"H.264 (libx264)", "H.265 (libx265)"});
            audioCodec_->clear();
            audioCodec_->addItems({"AAC"});
        }
        updateDerived();
    });
    connect(codec_, &QComboBox::currentTextChanged, this, [this] { updateDerived(); });
    connect(audioCodec_, &QComboBox::currentTextChanged, this, [this] { updateDerived(); });

    scroll->setWidget(content);
    into->addWidget(scroll, 1);
}

void DeliverPanel::buildQueue(QVBoxLayout* into) {
    auto* header = new QWidget(this);
    header->setObjectName("deliver-header");
    header->setFixedHeight(32);
    auto* headerRow = new QHBoxLayout(header);
    headerRow->setContentsMargins(12, 0, 8, 0);
    headerRow->setSpacing(8);
    auto* title = new QLabel("Render queue", header);
    title->setStyleSheet("font-weight:600;font-size:12px");
    queueSummary_ = new QLabel(header);
    queueSummary_->setStyleSheet(
        QString("color:%1;font-size:11px").arg(theme::textAt(0.40).name(QColor::HexRgb)));
    auto* clear = new QPushButton("Clear finished", header);
    clear->setProperty("flat", true);
    clear->setFixedHeight(22);
    connect(clear, &QPushButton::clicked, this, [this] {
        // Only what is finished: a queue that cleared a job somebody was
        // waiting on would be a queue nobody trusts.
        jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(),
                                   [](const std::unique_ptr<Job>& job) {
                                       return job->state == Job::State::Done ||
                                              job->state == Job::State::Failed;
                                   }),
                    jobs_.end());
        active_ = -1;
        rebuildQueueView();
    });
    headerRow->addWidget(title);
    headerRow->addWidget(queueSummary_);
    headerRow->addStretch(1);
    headerRow->addWidget(clear);
    into->addWidget(header);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget(scroll);
    queueLayout_ = new QVBoxLayout(content);
    queueLayout_->setContentsMargins(10, 10, 10, 10);
    queueLayout_->setSpacing(8);
    queueLayout_->addStretch(1);
    scroll->setWidget(content);
    into->addWidget(scroll, 1);

    auto* machine = new QWidget(this);
    auto* machineColumn = new QVBoxLayout(machine);
    machineColumn->setContentsMargins(12, 10, 12, 10);
    machineColumn->setSpacing(6);
    machineColumn->addWidget(sectionLabel("Machine", machine));
    const auto stat = [&](QLabel** slot) {
        *slot = new QLabel("—", machine);
        (*slot)->setStyleSheet(
            QString("color:%1;font-size:11px").arg(theme::textAt(0.52).name(QColor::HexRgb)));
        machineColumn->addWidget(*slot);
    };
    stat(&machineSpace_);
    stat(&machineJob_);
    stat(&machineSpeed_);

    auto* buttons = new QHBoxLayout;
    auto* openFolder = new QPushButton("Open destination", machine);
    openFolder->setProperty("flat", true);
    openFolder->setFixedHeight(28);
    connect(openFolder, &QPushButton::clicked, this, [this] {
        static_cast<void>(QDesktopServices::openUrl(QUrl::fromLocalFile(folder_->text())));
    });
    buttons->addWidget(openFolder, 1);
    machineColumn->addLayout(buttons);
    into->addWidget(machine);

    rebuildQueueView();
}

// --- what the controls mean --------------------------------------------------

const model::Sequence* DeliverPanel::sequence() const {
    return project_ != nullptr ? project_->findSequence(sequenceId_) : nullptr;
}

void DeliverPanel::setProject(const model::Project* project, model::SequenceId sequence) {
    project_ = project;
    sequenceId_ = sequence;
    refresh();
}

void DeliverPanel::setPlayhead(const time::RationalTime& position) {
    if (playhead_ == position) {
        return;
    }
    playhead_ = position;
    if (range_ != 0) {
        updateDerived();
    }
}

void DeliverPanel::refresh() {
    updateDerived();
}

void DeliverPanel::applyPreset(int index) {
    if (index < 0 || index >= static_cast<int>(presets_.size())) {
        return;
    }
    preset_ = index;
    const Preset& preset = presets_[static_cast<std::size_t>(index)];

    container_->setCurrentText(preset.extension == "mov" ? "MOV" : "MP4");
    // currentTextChanged rebuilt the codec menu; pick the preset's entry out of
    // it by the encoder name it carries in brackets.
    for (int i = 0; i < codec_->count(); ++i) {
        if (codec_->itemText(i).contains(preset.videoCodec)) {
            codec_->setCurrentIndex(i);
            break;
        }
    }
    for (int i = 0; i < audioCodec_->count(); ++i) {
        if (audioCodec_->itemText(i).contains(preset.audioCodec, Qt::CaseInsensitive)) {
            audioCodec_->setCurrentIndex(i);
            break;
        }
    }
    quality_->setValue(static_cast<int>(preset.quality * 1000));

    for (QListWidget* list : presetLists_) {
        bool mine = false;
        for (int row = 0; row < list->count(); ++row) {
            if (list->item(row)->data(Qt::UserRole).toInt() == index) {
                list->setCurrentRow(row);
                mine = true;
            }
        }
        if (!mine) {
            list->clearSelection();
            list->setCurrentRow(-1);
        }
    }
    updateDerived();
}

std::pair<std::int64_t, std::int64_t> DeliverPanel::chosenRange() const {
    const model::Sequence* seq = sequence();
    if (seq == nullptr) {
        return {0, 0};
    }
    const std::int64_t total = seq->duration().frames();
    switch (range_) {
        case 1: {
            const std::int64_t from =
                std::clamp<std::int64_t>(playhead_.rescaledTo(seq->frameRate()).frames(), 0, total);
            return {from, total - from};
        }
        case 2: {
            // The marker region the playhead is in, or the next one after it.
            // A marker with no duration marks an instant rather than a range,
            // so it is skipped: there would be nothing to render.
            const model::Marker* best = nullptr;
            for (const model::Marker& marker : seq->markers()) {
                if (marker.range.duration().frames() <= 0) {
                    continue;
                }
                if (marker.range.contains(playhead_.rescaledTo(seq->frameRate()))) {
                    best = &marker;
                    break;
                }
                if (best == nullptr && marker.range.start() >= playhead_) {
                    best = &marker;
                }
            }
            if (best == nullptr) {
                return {0, total};
            }
            return {best->range.start().frames(), best->range.duration().frames()};
        }
        default:
            return {0, total};
    }
}

std::int64_t DeliverPanel::bitRate() const {
    const Preset& preset = presets_[static_cast<std::size_t>(preset_)];
    const model::Sequence* seq = sequence();
    if (!preset.takesBitRate || seq == nullptr) {
        return 0;
    }
    // Bits per pixel rather than a flat number: the same setting has to mean
    // the same picture at 720p and at 4K, and it is the pixel rate that
    // decides what a codec needs.
    const double quality = quality_->value() / 1000.0;
    const double bitsPerPixel = 0.015 + quality * 0.185;
    const double pixels = static_cast<double>(seq->width()) * static_cast<double>(seq->height());
    return static_cast<std::int64_t>(pixels * seq->frameRate().toDouble() * bitsPerPixel);
}

std::string DeliverPanel::outputPath() const {
    const QString name = fileName_->text().isEmpty() ? QString("delivery") : fileName_->text();
    const QString extension = container_->currentText().toLower();
    return (std::filesystem::path(folder_->text().toStdString()) /
            (name + "." + extension).toStdString())
        .string();
}

void DeliverPanel::setDestination(const QString& folder, const QString& name) {
    folder_->setText(folder);
    fileName_->setText(name);
    updateDerived();
}

int DeliverPanel::finishedCount() const {
    return static_cast<int>(std::count_if(
        jobs_.begin(), jobs_.end(),
        [](const std::unique_ptr<Job>& job) { return job->state == Job::State::Done; }));
}

QString DeliverPanel::lastMessage() const {
    for (auto job = jobs_.rbegin(); job != jobs_.rend(); ++job) {
        if (!(*job)->message.isEmpty()) {
            return (*job)->message;
        }
    }
    return {};
}

void DeliverPanel::chooseFolder() {
    const QString chosen = QFileDialog::getExistingDirectory(this, "Deliver to", folder_->text());
    if (!chosen.isEmpty()) {
        folder_->setText(chosen);
        updateDerived();
    }
}

void DeliverPanel::updateDerived() {
    // The columns are built in order and each connects signals as it goes, so
    // this can be reached before the last of them exists.
    if (machineSpace_ == nullptr) {
        return;
    }
    const Preset& preset = presets_[static_cast<std::size_t>(preset_)];
    presetName_->setText(preset.name);
    presetDescription_->setText(preset.description);
    fullPath_->setText(QString::fromStdString(outputPath()));

    const model::Sequence* seq = sequence();
    const auto [start, count] = chosenRange();
    if (seq != nullptr) {
        resolution_->setText(QString("%1 × %2").arg(seq->width()).arg(seq->height()));
        frameRate_->setText(QString("%1 fps").arg(seq->frameRate().toDouble(), 0, 'g', 5));
        audioRate_->setText(
            QString("%1 kHz").arg(seq->audioSampleRate().toDouble() / 1000.0, 0, 'g', 3));
        const bool dropFrame = time::supportsDropFrame(seq->frameRate());
        const time::Timecode span = time::timecodeFromFrames(count, seq->frameRate(), dropFrame);
        estimateDuration_->setText(QString::fromStdString(span.toString()));
        estimateColour_->setText(seq->output().transfer == media::TransferFunction::BT709
                                     ? QString("Rec.709")
                                     : QString("Rec.709 · rolled off"));
    }
    depth_->setText(preset.depth);

    const std::int64_t rate = bitRate();
    const bool variable = preset.takesBitRate;
    quality_->setEnabled(variable);
    static const QStringList kNames{"Draft", "Web", "Broadcast", "Master"};
    const int band = std::min(3, quality_->value() * 4 / 1000);
    qualityName_->setText(variable ? QString("Quality — %1").arg(kNames.at(band))
                                   : QString("Quality — fixed by the codec"));
    // Below a megabit the number is not zero, it is small -- and a 320×240
    // sequence at draft quality is exactly that.
    const QString rateText =
        rate >= 1000000 ? QString("%1 Mb/s").arg(static_cast<double>(rate) / 1e6, 0, 'f', 1)
                        : QString("%1 kb/s").arg(rate / 1000);
    qualityRate_->setText(variable ? rateText : QString("—"));
    estimateBitrate_->setText(variable ? rateText : QString("—"));

    if (seq != nullptr && variable && rate > 0) {
        const double seconds = static_cast<double>(count) / seq->frameRate().toDouble();
        estimateSize_->setText(humanSize(static_cast<double>(rate) * seconds / 8.0));
    } else {
        // ProRes is bounded by its profile rather than by a number we set, so
        // a size guessed here would be a number with nothing behind it.
        estimateSize_->setText("—");
    }

    audioCodec_->setEnabled(audioOn_->isChecked());
    audioChannels_->setEnabled(audioOn_->isChecked());
    audioRate_->setEnabled(audioOn_->isChecked());
    loudness_->setText(
        audioOn_->isChecked()
            ? QString("Sound is written as mixed. Loudness normalisation is a separate step "
                      "— Sequence ▸ Loudness — and is not applied at export.")
            : QString("No sound will be written."));

    // Free space where the file is going, which is the one machine number that
    // can be read honestly on every platform.
    std::error_code code;
    const auto space = std::filesystem::space(folder_->text().toStdString(), code);
    machineSpace_->setText(code ? QString("Destination unavailable")
                                : QString("%1 free on the destination")
                                      .arg(humanSize(static_cast<double>(space.available))));

    emit queueChanged();
}

QString DeliverPanel::rangeSummary() const {
    const model::Sequence* seq = sequence();
    const auto [start, count] = chosenRange();
    static const QStringList kNames{"Whole timeline", "Playhead to end", "Marker region"};
    if (seq == nullptr) {
        return kNames.at(range_);
    }
    const bool dropFrame = time::supportsDropFrame(seq->frameRate());
    return QString("%1 · %2").arg(
        kNames.at(range_),
        QString::fromStdString(
            time::timecodeFromFrames(count, seq->frameRate(), dropFrame).toString()));
}

QString DeliverPanel::statusSummary() const {
    const auto pending =
        std::count_if(jobs_.begin(), jobs_.end(), [](const std::unique_ptr<Job>& job) {
            return job->state == Job::State::Queued || job->state == Job::State::Rendering;
        });
    if (active_ >= 0 && active_ < static_cast<int>(jobs_.size())) {
        const Job& job = *jobs_[static_cast<std::size_t>(active_)];
        return QString("Rendering %1 — %2%")
            .arg(job.name)
            .arg(static_cast<int>(job.progress * 100));
    }
    if (pending > 0) {
        return QString("%1 job%2 waiting").arg(pending).arg(pending == 1 ? "" : "s");
    }
    return QString("Queue idle");
}

// --- the queue ---------------------------------------------------------------

void DeliverPanel::queueCurrent() {
    const model::Sequence* seq = sequence();
    if (project_ == nullptr || seq == nullptr) {
        return;
    }
    const auto [start, count] = chosenRange();
    if (count <= 0) {
        return;
    }
    const Preset& preset = presets_[static_cast<std::size_t>(preset_)];

    // The encoder name lives in brackets in the menu entry, so the words in
    // front of it can say what a person calls the codec.
    const auto encoderName = [](const QString& entry) {
        const qsizetype open = entry.indexOf('(');
        const qsizetype close = entry.indexOf(')');
        return open >= 0 && close > open ? entry.mid(open + 1, close - open - 1) : entry;
    };

    auto job = std::make_unique<Job>();
    job->path = outputPath();
    job->name = QFileInfo(QString::fromStdString(job->path)).fileName();
    job->spec = QString("%1 · %2 × %3")
                    .arg(codec_->currentText().section(' ', 0, 0))
                    .arg(seq->width())
                    .arg(seq->height());
    job->project = *project_;
    job->reveal = revealWhenDone_->isChecked();
    job->framesTotal = count;

    job->request.outputPath = job->path;
    job->request.sequence = sequenceId_;
    job->request.startFrame = start;
    job->request.frameCount = count;
    job->request.includeAudio = audioOn_->isChecked();
    job->request.preferGpu = useGpu_->isChecked();
    job->request.allowCopy = smartRender_->isChecked();
    job->request.videoCodec = encoderName(codec_->currentText()).toStdString();
    job->request.audioCodec = audioOn_->isChecked()
                                  ? encoderName(audioCodec_->currentText()).toLower().toStdString()
                                  : std::string{};
    job->request.videoBitRate = preset.takesBitRate ? bitRate() : 0;

    jobs_.push_back(std::move(job));
    rebuildQueueView();
}

void DeliverPanel::toggleRendering() {
    if (rendering_) {
        stopCurrent();
        return;
    }
    rendering_ = true;
    startNext();
    emit queueChanged();
}

void DeliverPanel::stopCurrent() {
    rendering_ = false;
    cancel_.store(true, std::memory_order_relaxed);
    // The job goes back on the queue rather than being marked failed: nothing
    // was wrong with it, and a partial file is not a delivery. Starting again
    // starts it again from the beginning, because an encoder cannot resume a
    // file it has already closed.
    emit queueChanged();
}

void DeliverPanel::startNext() {
    if (!rendering_ || worker_.joinable()) {
        return;
    }
    const auto next = std::find_if(jobs_.begin(), jobs_.end(), [](const std::unique_ptr<Job>& job) {
        return job->state == Job::State::Queued;
    });
    if (next == jobs_.end()) {
        rendering_ = false;
        active_ = -1;
        rebuildQueueView();
        return;
    }

    active_ = static_cast<int>(std::distance(jobs_.begin(), next));
    Job& job = **next;
    job.state = Job::State::Rendering;
    job.message.clear();
    cancel_.store(false, std::memory_order_relaxed);
    rebuildQueueView();

    const std::size_t index = static_cast<std::size_t>(active_);
    // The project is copied into the job when it is queued, so the worker reads
    // something nobody else can touch while it renders.
    worker_ = std::thread{[this, index] {
        const Job& queued = *jobs_[index];
        const auto keepGoing = [this] {
            return !cancel_.load(std::memory_order_relaxed) &&
                   !shuttingDown_.load(std::memory_order_relaxed);
        };
        const auto onProgress = [this, index](const platform::ffmpeg::RenderProgress& progress) {
            QMetaObject::invokeMethod(
                this,
                [this, index, progress] {
                    jobProgress(index, progress.framesDone, progress.framesTotal,
                                progress.elapsedSeconds);
                },
                Qt::QueuedConnection);
        };

        // The same font engine the preview uses, so what is delivered is what
        // was on screen. Constructed here because it belongs to this thread.
        platform::qtext::QtTextRasterizer text;
        platform::ffmpeg::RenderSummary summary;
        const Status status = platform::ffmpeg::renderSequence(
            queued.project, queued.request, onProgress, keepGoing, &summary, &text);
        QString message;
        if (status) {
            message = summary.copied
                          ? QString("Copied without re-encoding")
                          : QString("Re-encoded — %1")
                                .arg(QString::fromStdString(summary.copyReason.empty()
                                                                ? std::string{"nothing to copy"}
                                                                : summary.copyReason));
        } else {
            message = QString::fromStdString(status.error().toString());
            // A partial file looks exactly like a finished one until somebody
            // plays it.
            std::error_code code;
            std::filesystem::remove(queued.request.outputPath, code);
        }
        QMetaObject::invokeMethod(
            this,
            [this, index, ok = static_cast<bool>(status), message] {
                jobFinished(index, ok, message);
            },
            Qt::QueuedConnection);
    }};
}

void DeliverPanel::jobProgress(std::size_t index, std::int64_t done, std::int64_t total,
                               double elapsed) {
    if (index >= jobs_.size()) {
        return;
    }
    Job& job = *jobs_[index];
    job.framesDone = done;
    job.framesTotal = total;
    job.elapsed = elapsed;
    job.progress = total > 0 ? static_cast<double>(done) / static_cast<double>(total) : 0.0;

    // Ten times a second, not once a frame. The renderer reports every frame,
    // and rebuilding the queue's cards -- each one a widget with a stylesheet
    // to parse -- that often costs more than the encoding does. It also starves
    // the thread doing the actual work, which is how a four-second render came
    // to take longer than the self-test was willing to wait.
    if (progressClock_.isValid() && progressClock_.elapsed() < 100 && done < total) {
        return;
    }
    progressClock_.restart();
    rebuildQueueView();
}

void DeliverPanel::jobFinished(std::size_t index, bool ok, const QString& message) {
    if (worker_.joinable()) {
        worker_.join();
    }
    if (index < jobs_.size()) {
        Job& job = *jobs_[index];
        job.message = message;
        if (ok) {
            job.state = Job::State::Done;
            job.progress = 1.0;
            if (job.reveal) {
                static_cast<void>(QDesktopServices::openUrl(QUrl::fromLocalFile(
                    QFileInfo(QString::fromStdString(job.path)).absolutePath())));
            }
        } else if (cancel_.load(std::memory_order_relaxed)) {
            job.state = Job::State::Queued;
            job.progress = 0.0;
            job.message = "Stopped";
        } else {
            job.state = Job::State::Failed;
        }
    }
    active_ = -1;
    if (rendering_) {
        startNext();
    }
    rebuildQueueView();
}

void DeliverPanel::rebuildQueueView() {
    // Rebuilt rather than updated in place: a queue is a handful of cards, and
    // keeping widgets in step with a list that can be cleared from under them
    // is more code than making them again.
    while (queueLayout_->count() > 1) {
        QLayoutItem* item = queueLayout_->takeAt(0);
        if (QWidget* widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }

    int pending = 0;
    for (std::size_t i = 0; i < jobs_.size(); ++i) {
        const Job& job = *jobs_[i];
        if (job.state == Job::State::Queued || job.state == Job::State::Rendering) {
            ++pending;
        }

        QColor colour = theme::accent(300);
        QString status = "Queued";
        switch (job.state) {
            case Job::State::Rendering:
                status = QString("%1%").arg(static_cast<int>(job.progress * 100));
                break;
            case Job::State::Done:
                colour = QColor{92, 176, 108};
                status = "Done";
                break;
            case Job::State::Failed:
                colour = QColor{232, 96, 96};
                status = "Failed";
                break;
            case Job::State::Stopped:
            case Job::State::Queued:
            default:
                colour = theme::textAt(0.5);
                break;
        }

        auto* card = new QWidget(this);
        card->setStyleSheet(QString("background:%1;border:1px solid %2;border-radius:7px")
                                .arg(theme::bg().name(QColor::HexRgb),
                                     (job.state == Job::State::Rendering
                                          ? theme::mix(theme::bg(), theme::accent(), 0.26)
                                          : theme::divider())
                                         .name(QColor::HexRgb)));
        auto* column = new QVBoxLayout(card);
        column->setContentsMargins(10, 9, 10, 9);
        column->setSpacing(6);

        auto* top = new QHBoxLayout;
        auto* name = new QLabel(job.name, card);
        name->setStyleSheet("font-size:11.5px;border:none");
        auto* state = new QLabel(status, card);
        state->setStyleSheet(
            QString("font-size:10px;border:none;color:%1").arg(colour.name(QColor::HexRgb)));
        top->addWidget(name, 1);
        top->addWidget(state);
        column->addLayout(top);

        auto* bar = new QProgressBar(card);
        bar->setRange(0, 1000);
        bar->setValue(static_cast<int>(job.progress * 1000));
        bar->setFixedHeight(6);
        bar->setTextVisible(false);
        bar->setStyleSheet(
            QString("QProgressBar{border:none;border-radius:3px;background:%1}"
                    "QProgressBar::chunk{border-radius:3px;background:%2}")
                .arg(theme::mix(theme::bg(), theme::text(), 0.08).name(QColor::HexRgb),
                     colour.name(QColor::HexRgb)));
        column->addWidget(bar);

        auto* foot = new QHBoxLayout;
        auto* spec = new QLabel(job.spec, card);
        spec->setStyleSheet(QString("font-size:10px;border:none;color:%1")
                                .arg(theme::textAt(0.40).name(QColor::HexRgb)));
        QString right = job.message;
        if (job.state == Job::State::Rendering && job.elapsed > 0.5 && job.progress > 0.01) {
            // From what this render has actually done, not from a guess made
            // before it started: the only honest estimate available.
            const double remaining = job.elapsed * (1.0 - job.progress) / job.progress;
            right = QString("%1 min left").arg(std::max(1.0, remaining / 60.0), 0, 'f', 0);
        }
        auto* note = new QLabel(right, card);
        note->setStyleSheet(QString("font-size:10px;border:none;color:%1")
                                .arg(theme::textAt(0.40).name(QColor::HexRgb)));
        note->setToolTip(job.message);
        foot->addWidget(spec, 1);
        foot->addWidget(note);
        column->addLayout(foot);

        queueLayout_->insertWidget(static_cast<int>(i), card);
    }

    if (jobs_.empty()) {
        auto* empty = new QLabel(
            "Nothing queued.\n\nAdd to queue puts the settings on the left "
            "here; Start render works through them one at a time.",
            this);
        empty->setWordWrap(true);
        empty->setAlignment(Qt::AlignTop);
        empty->setStyleSheet(
            QString("color:%1;font-size:11px").arg(theme::textAt(0.38).name(QColor::HexRgb)));
        queueLayout_->insertWidget(0, empty);
    }

    queueSummary_->setText(
        jobs_.empty() ? QString("empty")
                      : (pending > 0 ? QString("%1 pending").arg(pending) : QString("all done")));
    if (active_ >= 0 && active_ < static_cast<int>(jobs_.size())) {
        const Job& job = *jobs_[static_cast<std::size_t>(active_)];
        machineJob_->setText(QString("Frame %1 of %2").arg(job.framesDone).arg(job.framesTotal));
        const double fps =
            job.elapsed > 0.0 ? static_cast<double>(job.framesDone) / job.elapsed : 0.0;
        machineSpeed_->setText(
            QString("%1 fps · %2 s elapsed").arg(fps, 0, 'f', 1).arg(job.elapsed, 0, 'f', 0));
    } else {
        machineJob_->setText(rendering_ ? QString("Starting…") : QString("Nothing rendering"));
        machineSpeed_->setText(
            QString("Encoder: %1").arg(useGpu_->isChecked() ? "GPU where available" : "CPU"));
    }
    emit queueChanged();
}

}  // namespace zaro::app
