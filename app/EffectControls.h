#pragma once

#include <QWidget>
#include <array>
#include <functional>
#include <optional>
#include <vector>

#include "zaro/core/edit/CommandStack.h"
#include "zaro/core/edit/Operations.h"
#include "zaro/core/model/ClipKind.h"
#include "zaro/core/model/Project.h"
#include "zaro/core/render/FrameSource.h"

#include "CurveEditor.h"
#include "HueBand.h"

class QCheckBox;
class QFontComboBox;
class QListWidget;
class QPlainTextEdit;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QPushButton;
class QToolButton;
class QLabel;
class QButtonGroup;
class QFrame;
class QScrollArea;
class QSlider;

#include "zaro/ui/SequenceBinding.h"

namespace zaro::app {

/// Effect Controls: the parameters of whichever clip is selected.
///
/// Every change goes through the command stack with a merge key, so dragging a
/// value is one undo step rather than one per pixel, and nothing here can move
/// a clip or collide with a neighbour.
///
/// The panel is driven *from* the model rather than holding its own copy: after
/// any edit, and after undo, it re-reads what the clip actually says. A panel
/// that trusted its own widgets would drift out of step the first time
/// something changed the clip from elsewhere.
class EffectControls : public QWidget, public ui::SequenceBound {
    Q_OBJECT

public:
    explicit EffectControls(QWidget* parent = nullptr);

    void bind(const ui::SequenceBinding& binding) override;

    /// Show this clip's parameters. An invalid id clears the panel.
    void setSelection(model::TrackId track, model::ClipId clip);

    /// Show a whole selection, primary first.
    ///
    /// With more than one clip the panel keeps only the controls it can write
    /// to all of them -- the parameters, the blend, the level, the repair --
    /// and puts the rest away. A mask, a grade chain and an effect stack are
    /// each a piece of work on one clip, and a panel that showed one clip's
    /// while claiming to describe five would be writing to the one whose name
    /// is not on the header.
    void setSelection(const std::vector<edit::ClipRef>& clips);

    /// Which of the panel's three pages is up.
    ///
    /// A page rather than a filter: the design's header is three tabs, and a
    /// tab that only greys things out is a filter wearing a tab's clothes.
    /// What each one holds is the answer to a different question -- what does
    /// this clip look like, what does it sound like, and what *is* it -- and
    /// those were previously one column somebody scrolled through.
    enum class Pane { Inspector, Audio, Info };
    void setPane(Pane pane);
    [[nodiscard]] Pane pane() const noexcept { return pane_; }

    /// Scroll to a stage of the grade chain: 0 primary, 1 curves, 2 secondary,
    /// 3 look. The Color workspace draws that chain as nodes, and clicking one
    /// has to land somewhere -- this is where. The order matches
    /// `GradeNodes::Stage`, which is the render order.
    void revealStage(int stage);

    /// Show whether the pen is out. Called back when the overlay turns it off
    /// by itself -- the path closed, or was abandoned -- so the button and the
    /// picture never disagree about what a click will do.
    void setDrawingMask(bool drawing);

    /// Where the dialogue is read from when ducking. Not owned.
    void setAudioSource(render::AudioSource* audio) { audio_ = audio; }

    /// Where the playhead is.
    ///
    /// An animated parameter has no single value, so the panel shows the value
    /// at this moment and writes keyframes here. Without it the panel would
    /// have to invent a time, and every keyframe would land in the same place.
    void setPosition(const time::RationalTime& position);

    /// Re-read the model, after an undo or an edit made elsewhere.
    void refresh();

signals:
    /// A parameter changed, so anything showing the picture needs repainting.
    void edited();

    /// Keyframes were added, removed or moved, so the timeline's keyframe lane
    /// needs redrawing even though no picture changed.
    void keyframesChanged();

    /// The pen was turned on or off. The panel does not own the overlay that
    /// draws on the picture, so it asks rather than reaching for it.
    void drawMaskRequested(bool drawing);

    /// Pin this clip to whatever is under it, or let it go. The panel knows
    /// the clip but not what is beneath it at the playhead, which is a
    /// question about the whole sequence.
    void pinRequested();
    void unpinRequested();

    /// Follow the mask through the rest of the clip. The panel has the mask
    /// but not the pictures, so this is asked for rather than done here.
    void trackMaskRequested();

    /// Recompose this clip for the sequence's shape.
    void reframeRequested();

    /// Show the sequence a nested clip is made of. The panel knows which
    /// sequence it is but not which one the window is currently showing, and
    /// changing that is a decision about the whole window.
    void openSequenceRequested(model::SequenceId sequence);

    /// Hold this clip still, or throw the analysis away. The panel has neither
    /// the frames nor the decoder, so it asks.
    void stabiliseRequested();
    void clearStabilisationRequested();

private:
    /// One animatable parameter: its slider and spin box, its stopwatch, and
    /// the button that adds or removes a keyframe at the playhead.
    struct Row {
        model::Param param{};
        /// The form this row was added to, and the widget holding its controls.
        /// Kept so a row can be hidden on its own: an adjustment layer has an
        /// opacity and a blend mode but no position, and greying out four
        /// fields it can never use says less than not drawing them.
        ///
        /// Hidden through `QFormLayout::setRowVisible` rather than by hiding
        /// the two widgets, because a form leaves the row's spacing behind
        /// when its contents are merely invisible -- which reads as a gap
        /// somebody forgot to fill.
        QFormLayout* form{nullptr};
        QWidget* line{nullptr};
        /// This row's name, so it can be marked when the clips in a
        /// multi-selection do not agree about the value.
        QLabel* label{nullptr};
        QSlider* slider{nullptr};
        QDoubleSpinBox* spin{nullptr};
        QToolButton* stopwatch{nullptr};
        QToolButton* keyframe{nullptr};
        /// What this row's slider spans, which is not always what its spin box
        /// allows. See `SliderSpan`.
        double sliderLo{0.0};
        double sliderHi{1.0};
    };

    /// What a row's slider spans, when that is not what its spin box allows.
    ///
    /// Most parameters have one range and it is both: opacity is 0 to 1
    /// wherever you ask. The exceptions are the ones whose spin box range is a
    /// guard rail rather than a working range -- position is clamped at
    /// ±100000px so that a number nobody meant cannot corrupt a transform, and
    /// a slider across two hundred thousand pixels moves by six hundred of them
    /// per pixel of travel, which is not a control. So the slider covers what
    /// people actually work in and the spin box still takes the rest.
    ///
    /// The cost is that a value outside the span pins the knob to the end
    /// while the number beside it says something else. That is the honest
    /// display of "further than this control goes", and it is why the number
    /// is next to it.
    struct SliderSpan {
        double lo{0.0};
        double hi{1.0};
    };

    // Construction. The panel is eight groups of parameters and a scroll
    // area around them; the constructor was six hundred and forty-six lines
    // because it built all of them in one go. Split in strict source order,
    // so the widgets are made and the rows are added exactly as before.
    //
    // The colour group is the one seam that is not tidy: the wheels and the
    // vignette belong in it, but are built after the three groups that follow
    // it, so buildColourGroup hands its form back and the constructor passes
    // it on. Straightening that means changing the order widgets are created
    // in, which changes tab order -- worth doing, separately and on purpose.

    /// The spin boxes, the blend list and the enable box: every parameter
    /// widget the groups below arrange, made before any of them.
    void createParameterWidgets();
    void buildMotionGroup();
    /// Returns the colour group's form, which addWheelsTo and addVignetteTo
    /// finish once the groups built between them are done.
    QFormLayout* buildColourGroup();
    void buildSecondaryGroup();
    void buildMaskGroup();
    void buildEffectsGroup();
    void addWheelsTo(QFormLayout* colourForm);
    void addVignetteTo(QFormLayout* colourForm);
    void buildKeyGroup();
    void buildGraphicGroup();
    /// The typography of a text layer: what it says, and in what.
    ///
    /// Its own group beside the graphic one rather than inside it, because the
    /// box and the fill are shared with a shape and the typeface is not. What
    /// a title needs is the union of the two, and what a rectangle needs is
    /// the first of them.
    void buildTextGroup();
    /// The cameras of a multicam clip: which one is live, when to cut to
    /// another, and how far into each the group's zero point is.
    ///
    /// Switching an angle is a cut, which is why this cannot simply set a
    /// field: the clip is split and the part after takes the new angle. The
    /// sync offsets are not a cut and are the thing nothing else in the
    /// program can currently set at all.
    void buildAnglesGroup();
    /// The one control a nested clip has that no other kind does: a way in.
    void buildNestedGroup();
    void buildAudioGroup();
    /// The sticky header: the three tabs, the reset button beside them, and
    /// the row underneath that says which clip this is.
    ///
    /// Outside the scroll area, unlike everything else. What it says is what
    /// the rest of the panel is *about*, and a heading that scrolls off leaves
    /// a column of numbers belonging to a clip you can no longer see the name
    /// of.
    void buildHeader();
    /// The Info page: what this clip is, rather than anything to change.
    void buildInfoGroup();
    /// The groups into a scrolled column, then the connections that need
    /// every widget to exist first.
    void assemblePanel();

    /// Show the groups this page owns and hide the rest.
    ///
    /// One place decides, because two would disagree. Every group has its own
    /// reason to be hidden -- a mask group means nothing for a sound clip, a
    /// graphic group nothing for a clip that is not one -- and those reasons
    /// are recorded as the `shows` flags below while the clip is read, then
    /// combined with the page here.
    void applyPaneVisibility();
    /// Fill the identity row and the Info page from the selected clip.
    void applyIdentity();
    /// Cut the two identity lines to the width they have.
    ///
    /// A clip name is as long as somebody's camera made it and the panel is
    /// 300px, so one of the two has to give. Re-cut on the label's own resize
    /// rather than the panel's: what the text has to fit is the space the
    /// layout ended up giving it, which is not known until it has.
    void elideIdentity();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    /// Put the page's parameters back to their defaults, undoably.
    void resetPane();

    [[nodiscard]] const model::Clip* selectedClip() const;
    /// Any clip in the sequence this panel is bound to, by track and id. The
    /// writers need it because a multi-selection edit reads each clip's own
    /// state, not the primary's.
    [[nodiscard]] const model::Clip* clipAt(model::TrackId track, model::ClipId clip) const;
    /// The playhead in the selected clip's source time, or nothing if the
    /// playhead is not over the clip. A keyframe outside the clip's own range
    /// is unreachable and undeletable from the panel, so the buttons that would
    /// create one are disabled there instead.
    [[nodiscard]] std::optional<time::RationalTime> keyframeTime() const;
    void addRow(QFormLayout* form, const QString& label, model::Param param, QDoubleSpinBox* spin,
                std::optional<SliderSpan> span = {});
    void pushParameter(model::Param param, double value);
    void toggleAnimated(model::Param param, bool on);
    void toggleKeyframe(model::Param param);
    void applyKeyframeButtons();
    void pushTransform();
    void pushAudio();
    void pushColor();
    void pushCurves(const model::ToneCurves& curves, bool committed);
    void pushSecondary();
    void pushLut(const model::LutRef& lut);
    void pushGraphic();
    void pushMask();
    /// The secondary as the widgets currently describe it.
    [[nodiscard]] model::Secondary secondaryFromWidgets() const;
    void applyToWidgets();
    /// Put every row's knob where its value is.
    ///
    /// Explicit rather than left to the spin box's `valueChanged`, because that
    /// does not fire when a value is written that the box already held -- which
    /// is most of them, most of the time. Relying on it leaves every parameter
    /// still at its default showing a knob parked at the left end, saying
    /// something the number beside it flatly contradicts.
    void applySliders();
    void setEditingEnabled(bool enabled);

    model::Project* project_{nullptr};
    model::SequenceId sequenceId_;
    edit::CommandStack* commands_{nullptr};
    model::TrackId track_;
    model::ClipId clip_;
    /// The rest of the selection, if there is one. The primary is `track_` and
    /// `clip_` and is not repeated here.
    std::vector<edit::ClipRef> others_;
    [[nodiscard]] std::size_t selectionCount() const noexcept { return others_.size() + 1; }
    /// Run an edit against every selected clip, as one undo step.
    ///
    /// `build` is handed each clip in turn -- the primary first -- and returns
    /// the operation for it, or nothing if there is none to make. It is handed
    /// the ids rather than the clip because most of these read the clip's
    /// current state, and that state is different for each of them.
    void applyToSelection(
        const std::function<Result<edit::CommandPtr>(model::TrackId, model::ClipId)>& build);
    /// Whether every selected clip agrees about a parameter's value at the
    /// playhead. A row whose clips disagree is marked rather than silently
    /// showing one of them.
    [[nodiscard]] bool selectionAgreesOn(model::Param param) const;
    /// Put a `*` on the rows the selection does not agree about, and take it
    /// off again when it does.
    void markDisagreements();
    time::RationalTime position_;
    std::vector<Row> rows_;

    /// True while the panel is writing values into its own widgets, so their
    /// change signals do not bounce straight back into the model.
    bool updating_{false};

    /// True while a row's slider and its spin box are copying a value to each
    /// other, so the second one does not send it back.
    ///
    /// Distinct from `updating_`, which stops a value reaching the model at
    /// all. This one has to let it through: dragging a slider *is* an edit, and
    /// it reaches the model by way of the spin box, which is the one place that
    /// rounds and clamps a value the way the panel displays it.
    bool syncing_{false};

    Pane pane_{Pane::Inspector};
    QFrame* tabBar_{nullptr};
    QFrame* identityRow_{nullptr};
    QButtonGroup* tabs_{nullptr};
    QPushButton* inspectorTab_{nullptr};
    QPushButton* audioTab_{nullptr};
    QPushButton* infoTab_{nullptr};
    QPushButton* resetButton_{nullptr};
    /// The 52x32 tile the design puts beside the clip's name. A glyph for what
    /// kind of thing this is, not a thumbnail: a frame of the clip would want a
    /// decode, and the header has to be right the instant a selection changes.
    QLabel* identityTile_{nullptr};
    QLabel* identityName_{nullptr};
    QLabel* identityMeta_{nullptr};
    /// What those two say before it is cut to fit.
    QString identityNameFull_;
    QString identityMetaFull_;
    QWidget* infoGroup_{nullptr};
    /// Held so rows the selected kind has no answer for can be hidden rather
    /// than filled with a dash. See `applyIdentity`.
    QFormLayout* infoForm_{nullptr};
    /// The Info page's right-hand column, in the order `applyIdentity` fills
    /// them. Labels rather than fields: this page answers questions, and a
    /// disabled spin box answers them worse than a line of text.
    std::vector<QLabel*> infoValues_;

    /// What the selected clip is, or nothing if there is no selection.
    ///
    /// One value where there were four booleans. The booleans were each a
    /// different half-answer to the same question -- is this picture, is it
    /// sound, is it generated, can it be masked -- and every new kind of clip
    /// meant another one, set in one place and read in another. See
    /// `model::clipKindOf`, which is the whole answer, and `groupsFor` in the
    /// implementation, which is what each answer shows.
    std::optional<model::ClipKind> kind_;

    QDoubleSpinBox* positionX_{nullptr};
    QDoubleSpinBox* positionY_{nullptr};
    QDoubleSpinBox* scaleX_{nullptr};
    QDoubleSpinBox* scaleY_{nullptr};
    QDoubleSpinBox* rotation_{nullptr};
    QDoubleSpinBox* anchorX_{nullptr};
    QDoubleSpinBox* anchorY_{nullptr};
    QDoubleSpinBox* opacity_{nullptr};
    QComboBox* blend_{nullptr};
    QDoubleSpinBox* gain_{nullptr};
    QDoubleSpinBox* pan_{nullptr};
    QDoubleSpinBox* temperature_{nullptr};
    QDoubleSpinBox* tint_{nullptr};
    QDoubleSpinBox* exposure_{nullptr};
    QDoubleSpinBox* contrast_{nullptr};
    QDoubleSpinBox* saturation_{nullptr};
    QCheckBox* enabled_{nullptr};
    QWidget* videoGroup_{nullptr};
    CurveEditor* curves_{nullptr};
    /// Held so `revealStage` can scroll to it, and so the panel can grey it
    /// out and hide it for a clip that is not video.
    QWidget* colourGroup_{nullptr};
    /// The colour group's form and the rows in it that belong to one clip
    /// rather than to a value: the curve editor, the LUT, the wheels, the
    /// vignette. Hidden for a multi-selection -- see `applyPaneVisibility`.
    QFormLayout* colourForm_{nullptr};
    QWidget* lutRow_{nullptr};
    QWidget* wheelsBox_{nullptr};
    QScrollArea* scroll_{nullptr};
    QWidget* maskGroup_{nullptr};
    QComboBox* maskShape_{nullptr};
    QDoubleSpinBox* maskWidth_{nullptr};
    QDoubleSpinBox* maskHeight_{nullptr};
    QDoubleSpinBox* maskX_{nullptr};
    QDoubleSpinBox* maskY_{nullptr};
    QDoubleSpinBox* maskCorner_{nullptr};
    QDoubleSpinBox* maskFeather_{nullptr};
    QCheckBox* maskInverted_{nullptr};
    QPushButton* maskToPath_{nullptr};
    QPushButton* maskDraw_{nullptr};
    QPushButton* maskTrack_{nullptr};
    QDoubleSpinBox* introSeconds_{nullptr};
    QDoubleSpinBox* outroSeconds_{nullptr};
    void pushResponsive();
    QPushButton* pin_{nullptr};
    QPushButton* unpin_{nullptr};
    QPushButton* reframe_{nullptr};
    QPushButton* stabilise_{nullptr};
    QPushButton* unstabilise_{nullptr};
    void convertMaskToPath();
    QWidget* graphicGroup_{nullptr};
    QFormLayout* graphicForm_{nullptr};
    QComboBox* shapeKind_{nullptr};
    QDoubleSpinBox* shapeWidth_{nullptr};
    QDoubleSpinBox* shapeHeight_{nullptr};
    QDoubleSpinBox* shapeCorner_{nullptr};
    QDoubleSpinBox* shapeFeather_{nullptr};
    QDoubleSpinBox* shapeRed_{nullptr};
    QDoubleSpinBox* shapeGreen_{nullptr};
    QDoubleSpinBox* shapeBlue_{nullptr};
    /// How transparent the fill is. Coverage rather than brightness: a shape at
    /// 0.5 is a half-transparent shape, not a darker one. Distinct from the
    /// clip's own opacity, which fades the shape *and* anything the graphic
    /// draws inside it.
    QDoubleSpinBox* shapeAlpha_{nullptr};
    QWidget* textGroup_{nullptr};
    QPlainTextEdit* textBody_{nullptr};
    QFontComboBox* textFamily_{nullptr};
    QDoubleSpinBox* textSize_{nullptr};
    QCheckBox* textBold_{nullptr};
    QCheckBox* textItalic_{nullptr};
    QComboBox* textAlign_{nullptr};
    /// Send what the text widgets say to the model. The same command as
    /// `pushGraphic` -- a title's words and its box are one `model::Graphic` --
    /// so the two share a writer and neither can drop what the other set.
    void pushText();
    /// Send the typed words, if any have changed since the last time. Called
    /// when the field loses focus. See `eventFilter`.
    void commitText();
    /// Whether the text field holds something the model has not been told
    /// about yet.
    bool textDirty_{false};

    /// The rows of Motion that only mean something for a clip with a file
    /// behind it: pinning, auto-reframe, stabilisation, and retiming. Held as
    /// widgets rather than layouts so they can be hidden for a clip that
    /// generates what it shows.
    QFormLayout* motionMediaForm_{nullptr};
    QWidget* pinRow_{nullptr};
    QWidget* stabiliseRow_{nullptr};
    QWidget* remapRow_{nullptr};
    QWidget* speedRow_{nullptr};

    /// How fast the clip plays, as a percentage, and which way.
    ///
    /// Not stored on the clip -- speed is the ratio between its two ranges, so
    /// this reads `Clip::speed` and writes through `edit::makeSetSpeed`, which
    /// restretches the timeline range and ripples what follows. Which is why
    /// it is not a `Row`: it has no keyframes, and it changes how long the clip
    /// is rather than what it looks like.
    QDoubleSpinBox* speed_{nullptr};
    QCheckBox* reverse_{nullptr};
    void pushSpeed();

    QWidget* anglesGroup_{nullptr};
    QListWidget* angleList_{nullptr};
    QDoubleSpinBox* angleOffset_{nullptr};
    QPushButton* angleSwitch_{nullptr};
    /// Fill the angle list from the clip, and put the offset field on whichever
    /// row is picked.
    void showAngles();
    void pushAngleOffset();
    void switchToAngle();

    QWidget* nestedGroup_{nullptr};
    QLabel* nestedName_{nullptr};
    QPushButton* nestedOpen_{nullptr};

    QWidget* secondaryGroup_{nullptr};
    QPushButton* lutLoad_{nullptr};
    QPushButton* lutClear_{nullptr};
    QPushButton* lutSave_{nullptr};
    void saveLookAsCube();
    QLabel* lutName_{nullptr};
    QDoubleSpinBox* lutAmount_{nullptr};
    /// Time remapping: a switch rather than a stopwatch, because a remap that
    /// is not keyframed is not a remap -- see model::Param::TimeRemap.
    QCheckBox* timeRemap_{nullptr};
    QPushButton* freeze_{nullptr};

    /// The effect stack. A fixed pool of parameter rows, relabelled from
    /// model::parametersOf, so that adding an effect to the model is data
    /// rather than another widget here.
    static constexpr int kMaxEffectParams = 4;
    QListWidget* effectList_{nullptr};
    QComboBox* effectKind_{nullptr};
    QPushButton* effectAdd_{nullptr};
    QPushButton* effectRemove_{nullptr};
    QPushButton* effectUp_{nullptr};
    QPushButton* effectDown_{nullptr};
    QCheckBox* effectEnabled_{nullptr};
    std::array<QLabel*, kMaxEffectParams> effectParamLabels_{};
    std::array<QDoubleSpinBox*, kMaxEffectParams> effectParamSpins_{};
    std::array<QToolButton*, kMaxEffectParams> effectParamStopwatches_{};
    std::array<QToolButton*, kMaxEffectParams> effectParamKeyframes_{};
    /// Which parameter each row is showing at the moment. The rows are a pool
    /// that gets relabelled, so a handler cannot capture its parameter the way
    /// the fixed rows above do.
    std::array<model::EffectParam, kMaxEffectParams> effectParamOf_{};
    void toggleEffectAnimated(int row, bool on);
    void toggleEffectKeyframe(int row);
    QWidget* effectGroup_{nullptr};
    void pushEffects();
    void showEffects();
    bool applyEffectStack(const std::vector<model::Effect>& stack);

    /// Filtering and compression for the selected clip: the repair a track's
    /// channel strip cannot do, because a track's applies to everything on it.
    /// Two switches and their settings, written as one command -- see
    /// `edit::makeSetClipProcessing`.
    QWidget* processingGroup_{nullptr};
    QCheckBox* clipEqOn_{nullptr};
    QDoubleSpinBox* clipHighPass_{nullptr};
    QDoubleSpinBox* clipLowPass_{nullptr};
    QDoubleSpinBox* clipPeakHz_{nullptr};
    QDoubleSpinBox* clipPeakGain_{nullptr};
    QDoubleSpinBox* clipPeakQ_{nullptr};
    QCheckBox* clipCompressorOn_{nullptr};
    QDoubleSpinBox* clipThreshold_{nullptr};
    QDoubleSpinBox* clipRatio_{nullptr};
    QDoubleSpinBox* clipAttack_{nullptr};
    QDoubleSpinBox* clipRelease_{nullptr};
    QDoubleSpinBox* clipMakeup_{nullptr};
    void buildProcessingGroup();
    void pushProcessing();

    QComboBox* role_{nullptr};
    QPushButton* duck_{nullptr};
    render::AudioSource* audio_{nullptr};
    void pushRole();
    void duckUnderDialogue();

    /// Shadows, midtones and highlights: offset, power and slope per channel.
    std::array<QDoubleSpinBox*, 9> wheels_{};
    void pushWheels();

    QDoubleSpinBox* vignetteAmount_{nullptr};
    QDoubleSpinBox* vignetteMidpoint_{nullptr};
    QDoubleSpinBox* vignetteFeather_{nullptr};
    QDoubleSpinBox* vignetteRoundness_{nullptr};
    void pushVignette();

    QComboBox* keyKind_{nullptr};
    QDoubleSpinBox* keyRed_{nullptr};
    QDoubleSpinBox* keyGreen_{nullptr};
    QDoubleSpinBox* keyBlue_{nullptr};
    QDoubleSpinBox* keyTolerance_{nullptr};
    QDoubleSpinBox* keySoftness_{nullptr};
    QDoubleSpinBox* keySpill_{nullptr};
    QDoubleSpinBox* keyLumaLow_{nullptr};
    QDoubleSpinBox* keyLumaHigh_{nullptr};
    QCheckBox* keyShowMatte_{nullptr};
    QWidget* keyGroup_{nullptr};
    void pushKeyer();

    QCheckBox* qualifierOn_{nullptr};
    QCheckBox* showMask_{nullptr};
    HueBand* hueBand_{nullptr};
    QDoubleSpinBox* hueCentre_{nullptr};
    QDoubleSpinBox* hueWidth_{nullptr};
    QDoubleSpinBox* hueSoftness_{nullptr};
    QDoubleSpinBox* satLow_{nullptr};
    QDoubleSpinBox* satHigh_{nullptr};
    QDoubleSpinBox* lumaLow_{nullptr};
    QDoubleSpinBox* lumaHigh_{nullptr};
    QDoubleSpinBox* keyExposure_{nullptr};
    QDoubleSpinBox* keySaturation_{nullptr};
    QDoubleSpinBox* keyTemperature_{nullptr};
    QWidget* audioGroup_{nullptr};
};

}  // namespace zaro::app
