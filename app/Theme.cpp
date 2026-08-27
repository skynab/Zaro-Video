#include "Theme.h"

#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QPalette>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace zaro::app::theme {
namespace {

// The tokens, exactly as the design system writes them. Nothing else in the
// application names a colour by value.
constexpr QRgb kBg = 0xff161826;
constexpr QRgb kSurface = 0xff232532;
constexpr QRgb kWell = 0xff0d0e16;
constexpr QRgb kText = 0xffe9e9ed;
constexpr QRgb kAccent = 0xff9184d9;

constexpr std::array<QRgb, 9> kNeutralRamp{0xfff3f5fe, 0xffe4e7f5, 0xffcfd3e5,
                                           0xffb2b6ca, 0xff9397ab, 0xff75798c,
                                           0xff595d6c, 0xff3f424d, 0xff292b31};
constexpr std::array<QRgb, 9> kAccentRamp{0xfff5f4ff, 0xffe7e5fe, 0xffd2cefd,
                                          0xffb5abfc, 0xff968ae0, 0xff796cbf,
                                          0xff5d5294, 0xff423a6a, 0xff2b2741};

/// Step 100..900 to an index into a ramp, clamped.
int rampIndex(int step) {
    const int index = (std::clamp(step, 100, 900) / 100) - 1;
    return std::clamp(index, 0, 8);
}

/// A colour as `#rrggbb`, for pasting into the stylesheet.
QString hex(const QColor& colour) {
    return colour.name(QColor::HexRgb);
}

}  // namespace

QColor bg() {
    return QColor::fromRgba(kBg);
}
QColor surface() {
    return QColor::fromRgba(kSurface);
}
QColor well() {
    return QColor::fromRgba(kWell);
}
QColor text() {
    return QColor::fromRgba(kText);
}
QColor accent() {
    return QColor::fromRgba(kAccent);
}
QColor accent(int step) {
    return QColor::fromRgba(kAccentRamp[static_cast<std::size_t>(rampIndex(step))]);
}
QColor neutral(int step) {
    return QColor::fromRgba(kNeutralRamp[static_cast<std::size_t>(rampIndex(step))]);
}

QColor mix(const QColor& under, const QColor& over, double amount) {
    const double a = std::clamp(amount, 0.0, 1.0);
    const auto blend = [a](int u, int o) {
        return static_cast<int>(std::lround(u * (1.0 - a) + o * a));
    };
    return QColor{blend(under.red(), over.red()), blend(under.green(), over.green()),
                  blend(under.blue(), over.blue())};
}

QColor textAt(double fraction) {
    return mix(bg(), text(), fraction);
}

// The divider is the design's `color-mix(in srgb, text 16%, transparent)` --
// which over the window ground resolves to text at 16%.
QColor divider() {
    return textAt(0.16);
}

QString styleSheet() {
    const QString accentHex = hex(accent());
    const QString surfaceHex = hex(surface());
    const QString bgHex = hex(bg());
    const QString textHex = hex(text());
    const QString dividerHex = hex(divider());
    const QString hoverHex = hex(mix(surface(), text(), 0.08));
    const QString pressHex = hex(mix(surface(), accent(), 0.20));
    const QString selectHex = hex(mix(surface(), accent(), 0.18));
    const QString mutedHex = hex(textAt(0.55));

    // Written as one sheet rather than per-widget calls: a control's look
    // should not depend on which panel happened to construct it.
    return QString(R"(
QMenuBar { background: transparent; border: none; padding: 1px 4px; }
QMenuBar::item { background: transparent; padding: 3px 8px; border-radius: 5px; }
QMenuBar::item:selected, QMenuBar::item:pressed { background: %HOVER%; }

QMenu { background: %SURFACE%; border: 1px solid %DIVIDER%; border-radius: 8px; padding: 4px; }
QMenu::item { padding: 4px 22px 4px 10px; border-radius: 5px; }
QMenu::item:selected { background: %SELECT%; color: %ACCENT200%; }
QMenu::item:disabled { color: %MUTED%; }
QMenu::separator { height: 1px; background: %DIVIDER%; margin: 3px 8px; }
QMenu::indicator { width: 12px; height: 12px; margin-left: 6px; }

QPushButton {
    background: transparent; color: %TEXT%;
    border: 1px solid %DIVIDER%; border-radius: 8px;
    padding: 4px 10px; min-height: 22px;
}
QPushButton:hover { background: %HOVER%; }
QPushButton:pressed { background: %PRESS%; }
QPushButton:checked { background: %SELECT%; color: %ACCENT200%; border-color: %ACCENT%; }
QPushButton:disabled { color: %MUTED%; border-color: %DIVIDER%; }
QPushButton[accent="true"] { color: %ACCENT%; border-color: %ACCENT%; }
QPushButton[flat="true"] { border-color: transparent; color: %MUTED%; }
QPushButton[flat="true"]:hover { background: %HOVER%; color: %TEXT%; }

QToolButton {
    background: transparent; color: %MUTED%;
    border: 1px solid transparent; border-radius: 6px; padding: 2px 4px;
}
QToolButton:hover { background: %HOVER%; color: %TEXT%; }
QToolButton:pressed { background: %PRESS%; }
QToolButton:checked { background: %SELECT%; color: %ACCENT200%; }

QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {
    background: %BG%; color: %TEXT%; selection-background-color: %ACCENT700%;
    border: 1px solid %DIVIDER%; border-radius: 6px;
    padding: 2px 6px; min-height: 22px;
}
QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover, QComboBox:hover { border-color: %NEUTRAL600%; }
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus { border-color: %ACCENT%; }
QComboBox::drop-down { border: none; width: 16px; }
QComboBox QAbstractItemView {
    background: %SURFACE%; border: 1px solid %DIVIDER%; border-radius: 6px;
    selection-background-color: %ACCENT800%; selection-color: %ACCENT100%; padding: 3px;
}
QSpinBox::up-button, QSpinBox::down-button,
QDoubleSpinBox::up-button, QDoubleSpinBox::down-button { width: 13px; border: none; background: transparent; }

QCheckBox, QRadioButton { spacing: 7px; }
QCheckBox::indicator, QRadioButton::indicator {
    width: 13px; height: 13px; border: 1px solid %DIVIDER%; background: %BG%;
}
QCheckBox::indicator { border-radius: 4px; }
QRadioButton::indicator { border-radius: 7px; }
QCheckBox::indicator:hover, QRadioButton::indicator:hover { border-color: %ACCENT%; }
QCheckBox::indicator:checked, QRadioButton::indicator:checked {
    background: %ACCENT600%; border-color: %ACCENT%;
}

QSlider::groove:horizontal { height: 3px; background: %NEUTRAL800%; border-radius: 2px; }
QSlider::sub-page:horizontal { background: %ACCENT600%; border-radius: 2px; }
QSlider::handle:horizontal {
    width: 9px; height: 9px; margin: -4px 0; border-radius: 5px; background: %ACCENT300%;
}
QSlider::handle:horizontal:hover { background: %ACCENT200%; }

QScrollBar:vertical { width: 9px; background: transparent; margin: 0; }
QScrollBar:horizontal { height: 9px; background: transparent; margin: 0; }
QScrollBar::handle { background: %NEUTRAL800%; border-radius: 5px; min-height: 24px; min-width: 24px; }
QScrollBar::handle:hover { background: %NEUTRAL700%; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QGroupBox {
    border: 1px solid %DIVIDER%; border-radius: 8px;
    /* The title sits in this margin; anything less and the border is drawn
       through the middle of the words. */
    margin-top: 18px; padding: 10px 8px 6px;
}
QGroupBox::title {
    subcontrol-origin: margin; subcontrol-position: top left; left: 10px; padding: 0 4px;
    color: %MUTED%; font-size: 10px;
}

QListWidget, QTreeWidget, QAbstractScrollArea {
    background: %BG%; border: 1px solid %DIVIDER%; border-radius: 8px;
}
QListWidget::item { padding: 3px 6px; border-radius: 6px; }
QListWidget::item:hover { background: %HOVER%; }
QListWidget::item:selected { background: %ACCENT800%; color: %ACCENT100%; }

QSplitter::handle { background: %DIVIDER%; }
QSplitter::handle:horizontal { width: 1px; }
QSplitter::handle:vertical { height: 1px; }
QSplitter::handle:hover { background: %ACCENT%; }

QProgressBar {
    border: 1px solid %DIVIDER%; border-radius: 5px; background: %BG%;
    max-height: 8px; text-align: center; color: transparent;
}
QProgressBar::chunk { background: %ACCENT600%; border-radius: 4px; }

QToolTip {
    background: %SURFACE%; color: %TEXT%;
    border: 1px solid %DIVIDER%; border-radius: 6px; padding: 3px 6px;
}
QLabel { background: transparent; }

/* --- the window's own chrome ------------------------------------------- */

#chrome-titlebar { background: %SURFACE%; border-bottom: 1px solid %DIVIDER%; }
#chrome-toolbar, #chrome-timeline-bar, #chrome-viewer-bar {
    background: %BG%; border-bottom: 1px solid %DIVIDER%;
}
#chrome-statusbar { background: %BG%; border-top: 1px solid %DIVIDER%; }
#chrome-statusbar QLabel, #chrome-toolbar QLabel[muted="true"],
#chrome-timeline-bar QLabel[muted="true"], #chrome-viewer-bar QLabel[muted="true"] {
    color: %MUTED%; font-size: 11px;
}
#chrome-brand { font-weight: 600; padding: 0 6px; }
#viewer-well { background: %WELL%; }
#deliver-side { background: %SURFACE%; }
#deliver-header { background: %SURFACE%; border-bottom: 1px solid %DIVIDER%; }
#timecode-big { color: %ACCENT300%; }
#segment-group { border: 1px solid %DIVIDER%; border-radius: 8px; }
#tab-group { background: %HOVER%; border-radius: 8px; }
#tab-group QPushButton { border-color: transparent; color: %MUTED%; }
#tab-group QPushButton:checked { background: %SURFACE%; color: %ACCENT200%; border-color: %DIVIDER%; }

/* --- the media pane ----------------------------------------------------- */

/* A surface panel against the window ground, with the divider on its edge
   rather than a splitter handle: the design draws one hairline there, and two
   would read as a gap. */
#bin-panel { background: %SURFACE%; border-right: 1px solid %DIVIDER%; }
#bin-tabbar, #bin-footer { background: transparent; }
#bin-tabbar { border-bottom: 1px solid %DIVIDER%; }
#bin-footer { border-top: 1px solid %DIVIDER%; color: %FAINT%; font-size: 10px; }
#bin-tab {
    border: none; background: transparent; color: %MUTED%;
    padding: 3px 9px; border-radius: 5px; min-height: 18px; font-size: 11px;
}
#bin-tab:hover { background: %BINHOVER%; color: %TEXT%; }
#bin-tab:checked { background: %ACCENTWASH%; color: %ACCENT300%; }
#bin-chip {
    border: 1px solid transparent; border-radius: 9px; background: %BINHOVER%;
    color: %MUTED%; padding: 1px 8px; min-height: 15px; font-size: 10px;
}
#bin-chip:hover { color: %TEXT%; }
#bin-chip:checked { background: %ACCENTWASH%; color: %ACCENT300%; }
#bin-chip[outline="true"] { background: transparent; border-color: %DIVIDER%; }
#bin-chip[outline="true"]:checked { background: %ACCENTWASH%; border-color: %ACCENT%; }
#bin-search { background: %BG%; padding: 3px 8px; min-height: 26px; }
#bin-glyph-button {
    border: 1px solid %DIVIDER%; border-radius: 6px; background: transparent;
    padding: 0; min-height: 26px;
}
#bin-glyph-button:hover { background: %BINHOVER%; }
#bin-glyph-button:checked { background: %ACCENTWASH%; border-color: %ACCENT%; }
/* No frame and no ground of its own: the rows are drawn by the delegate, and a
   sunken box inside a surface panel is a second panel. */
#bin-list { background: transparent; border: none; }
#bin-list::item { padding: 0; border-radius: 0; background: transparent; }
#bin-list::item:hover, #bin-list::item:selected { background: transparent; }

/* --- the Color workspace ------------------------------------------------ */

#gallery-panel, #color-palette { background: %SURFACE%; }
#gallery-panel { border-right: 1px solid %DIVIDER%; }
#color-palette { border-top: 1px solid %DIVIDER%; }
#gallery-heading { border-bottom: 1px solid %DIVIDER%; }
#gallery-title { font-weight: 500; font-size: 11px; }
#gallery-grid, #gallery-luts, #palette-list { background: transparent; border: none; }
#gallery-grid::item { border-radius: 5px; padding: 2px; color: %FAINT%; font-size: 9px; }
#gallery-grid::item:selected, #gallery-luts::item:selected, #palette-list::item:selected {
    background: %ACCENTWASH%; color: %ACCENT300%;
}
#gallery-luts::item, #palette-list::item {
    border-radius: 5px; padding: 4px 6px; color: %MUTED%; font-size: 11px;
}
#gallery-luts::item:hover, #palette-list::item:hover { background: %BINHOVER%; color: %TEXT%; }
#palette-list { border-right: 1px solid %DIVIDER%; padding: 10px 8px; }
/* The strip is a well, like the viewer: it is full of pictures. */
#clip-strip { background: %WELL%; border-top: 1px solid %DIVIDER%; }
#grade-nodes-box { background: %SURFACE%; border-bottom: 1px solid %DIVIDER%; }
#scopes-panel { background: transparent; }
#scope-tabs { background: %HOVER%; border-radius: 7px; }
#scope-tab {
    border: none; background: transparent; color: %MUTED%;
    padding: 3px 0; border-radius: 5px; min-height: 16px; font-size: 10px;
}
#scope-tab:hover { color: %TEXT%; }
#scope-tab:checked { background: %ACCENTWASH%; color: %ACCENT200%; }
#scope-readout { background: %SURFACE%; border: 1px solid %DIVIDER%; border-radius: 6px; }
#scope-readout-label {
    font-size: 9px; letter-spacing: 0.06em; color: %FAINT%; text-transform: uppercase;
}
#scope-readout-value { font-family: Menlo, monospace; font-size: 12px; }

/* --- the Audio workspace ------------------------------------------------ */

#audio-side, #loudness-panel, #channel-panel, #frame-thumb { background: %SURFACE%; }
#frame-thumb { border-bottom: 1px solid %DIVIDER%; }
#audio-side { border-right: 1px solid %DIVIDER%; }
#loudness-panel { border-bottom: 1px solid %DIVIDER%; }
#channel-panel { border-left: 1px solid %DIVIDER%; }
#channel-header, #mixer-header { border-bottom: 1px solid %DIVIDER%; background: %SURFACE%; }
#channel-heading {
    font-size: 10px; letter-spacing: 0.07em; text-transform: uppercase; color: %FAINT%;
}
#mixer-panel { background: %BG%; }
/* The console sits in a well, like the viewer: it is a row of instruments. */
#mixer-console { background: %WELL%; }
#loudness-measure { font-size: 10px; padding: 2px 8px; border-radius: 6px; }
)")
        .replace("%BG%", bgHex)
        .replace("%SURFACE%", surfaceHex)
        .replace("%WELL%", hex(well()))
        .replace("%TEXT%", textHex)
        .replace("%MUTED%", mutedHex)
        .replace("%DIVIDER%", dividerHex)
        .replace("%HOVER%", hoverHex)
        .replace("%PRESS%", pressHex)
        .replace("%SELECT%", selectHex)
        .replace("%ACCENT100%", hex(accent(100)))
        .replace("%ACCENT200%", hex(accent(200)))
        .replace("%ACCENT300%", hex(accent(300)))
        .replace("%ACCENT600%", hex(accent(600)))
        .replace("%ACCENT700%", hex(accent(700)))
        .replace("%ACCENT800%", hex(accent(800)))
        .replace("%NEUTRAL600%", hex(neutral(600)))
        .replace("%NEUTRAL700%", hex(neutral(700)))
        .replace("%NEUTRAL800%", hex(neutral(800)))
        .replace("%ACCENTWASH%", hex(mix(surface(), accent(), 0.16)))
        .replace("%BINHOVER%", hex(mix(surface(), text(), 0.08)))
        .replace("%FAINT%", hex(textAt(0.42)))
        .replace("%ACCENT%", accentHex);
}

void apply(QApplication& application) {
    // The palette as well as the sheet: dialogs and anything drawn by the style
    // rather than by a rule -- a menu shadow, a disabled label, the text cursor
    // -- read the palette, and a light one under a dark sheet shows up as white
    // flashes at exactly the moments nobody is looking.
    QPalette palette;
    palette.setColor(QPalette::Window, bg());
    palette.setColor(QPalette::WindowText, text());
    palette.setColor(QPalette::Base, bg());
    palette.setColor(QPalette::AlternateBase, surface());
    palette.setColor(QPalette::Text, text());
    palette.setColor(QPalette::Button, surface());
    palette.setColor(QPalette::ButtonText, text());
    palette.setColor(QPalette::BrightText, accent(200));
    palette.setColor(QPalette::Highlight, accent(700));
    palette.setColor(QPalette::HighlightedText, accent(100));
    palette.setColor(QPalette::ToolTipBase, surface());
    palette.setColor(QPalette::ToolTipText, text());
    palette.setColor(QPalette::PlaceholderText, textAt(0.45));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, textAt(0.38));
    palette.setColor(QPalette::Disabled, QPalette::Text, textAt(0.38));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, textAt(0.38));
    QApplication::setPalette(palette);

    // Inter is the system's face if it happens to be installed; asking for a
    // family that is not there costs an alias lookup and lands somewhere
    // arbitrary, so the fallback is named rather than guessed at.
    QFont font = QApplication::font();
    if (QFontDatabase::families().contains("Inter")) {
        font.setFamily("Inter");
    }
    font.setPointSizeF(11.5);
    QApplication::setFont(font);

    application.setStyleSheet(styleSheet());
}

}  // namespace zaro::app::theme
