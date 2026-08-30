#include "zaro/ui/Actions.h"

#include <algorithm>
#include <array>

namespace zaro::ui {
namespace {

/// The catalogue.
///
/// Defaults are Premiere's where Premiere has one, because those are what hands
/// already know. Where it has none -- the things this program has that that one
/// does not -- the rule is that single letters stay free: they fire while
/// somebody is typing into a field, so anything bound to one has to be worth
/// that risk. Playback and the mark keys are the exception, and they are the
/// exception in every editor.
constexpr std::array kActions{
    // --- File ---------------------------------------------------------------
    ActionInfo{"new-project", "New", "File", "Ctrl+N"},
    ActionInfo{"open-project", "Open…", "File", "Ctrl+O"},
    ActionInfo{"save-project", "Save", "File", "Ctrl+S"},
    ActionInfo{"save-project-as", "Save As…", "File", "Ctrl+Shift+S"},
    ActionInfo{"save-version", "Save a New Version", "File", "Ctrl+Alt+S"},
    ActionInfo{"open-version", "Open a Version…", "File", ""},
    ActionInfo{"import-media", "Import Media…", "File", "Ctrl+I"},
    ActionInfo{"browse-media", "Browse Media…", "File", "Ctrl+Shift+I"},
    ActionInfo{"relink-media", "Relink Media…", "File", ""},
    ActionInfo{"consolidate-media", "Consolidate Media…", "File", ""},
    ActionInfo{"export-sequence", "Export…", "File", "Ctrl+E"},
    ActionInfo{"export-otio", "Export OpenTimelineIO…", "File", ""},
    ActionInfo{"export-premiere", "Export Premiere XML…", "File", ""},
    ActionInfo{"import-premiere", "Import Premiere XML…", "File", ""},
    ActionInfo{"export-finalcut", "Export Final Cut Pro XML…", "File", ""},
    ActionInfo{"import-finalcut", "Import Final Cut Pro XML…", "File", ""},
    ActionInfo{"save-template", "Save Graphic as Template…", "File", ""},
    ActionInfo{"place-template", "Place Graphic Template…", "File", ""},
    ActionInfo{"close-window", "Close Window", "File", "Ctrl+W"},

    // --- Edit ---------------------------------------------------------------
    ActionInfo{"undo", "Undo", "Edit", "Ctrl+Z"},
    ActionInfo{"redo", "Redo", "Edit", "Ctrl+Shift+Z"},
    ActionInfo{"select-all", "Select All", "Edit", "Ctrl+A"},
    ActionInfo{"detect-scenes", "Detect Cuts in Selected Clip", "Edit", "Ctrl+D"},

    // --- Clip ---------------------------------------------------------------
    ActionInfo{"match-frame", "Match Frame", "Clip", "F"},
    ActionInfo{"make-subclip", "Make Subclip", "Clip", ""},
    ActionInfo{"proxies", "Proxies…", "Clip", ""},
    ActionInfo{"multicam", "Multicam…", "Clip", ""},
    ActionInfo{"captions", "Captions…", "Clip", ""},

    // --- Sequence -----------------------------------------------------------
    ActionInfo{"razor", "Razor at Playhead", "Sequence", "Ctrl+K"},
    ActionInfo{"add-dissolve", "Add Dissolve at Playhead", "Sequence", ""},
    ActionInfo{"render-range", "Render Range…", "Sequence", ""},
    ActionInfo{"delivery", "Delivery…", "Sequence", ""},
    ActionInfo{"loudness", "Loudness…", "Sequence", ""},
    ActionInfo{"insert-from-source", "Insert from Source", "Sequence", "Comma"},
    ActionInfo{"overwrite-from-source", "Overwrite from Source", "Sequence", "Period"},

    // --- Text and audio -----------------------------------------------------
    ActionInfo{"show-transcript", "Transcript…", "Text", "Ctrl+T"},
    ActionInfo{"fit-music", "Fit Music to the Picture", "Audio", ""},

    // --- Markers ------------------------------------------------------------
    ActionInfo{"add-marker", "Add Marker", "Marker", "M"},
    ActionInfo{"next-marker", "Go to Next Marker", "Marker", "Shift+Right"},
    ActionInfo{"previous-marker", "Go to Previous Marker", "Marker", "Shift+Left"},
    ActionInfo{"resolve-comment", "Resolve Comment Here", "Marker", "Shift+R"},
    ActionInfo{"export-review", "Export Review Notes…", "Marker", ""},

    // --- Effects ------------------------------------------------------------
    ActionInfo{"compare", "Hold This Frame to Compare", "Effects", ""},
    ActionInfo{"match-shot", "Match Selected Clip to Held Frame", "Effects", ""},

    // --- View ---------------------------------------------------------------
    ActionInfo{"zoom-in", "Zoom In", "View", "Ctrl+="},
    ActionInfo{"zoom-out", "Zoom Out", "View", "Ctrl+-"},
    ActionInfo{"zoom-fit", "Zoom to Fit", "View", "Ctrl+Shift+F"},
    ActionInfo{"safe-guides", "Safe Area Guides", "View", "Ctrl+'"},
    ActionInfo{"reset-panels", "Reset Panels", "Window", ""},
    ActionInfo{"hotkeys", "Keyboard Shortcuts…", "Window", ""},
    ActionInfo{"about", "About CutReel", "Window", ""},

    // --- Playback and marking ----------------------------------------------
    // The single letters, which are the ones every editor binds and every pair
    // of hands expects.
    ActionInfo{"play-pause", "Play / Pause", "Playback", "Space"},
    ActionInfo{"shuttle-back", "Shuttle Backwards", "Playback", "J"},
    ActionInfo{"shuttle-stop", "Stop Shuttling", "Playback", "K"},
    ActionInfo{"shuttle-forward", "Shuttle Forwards", "Playback", "L"},
    ActionInfo{"step-back", "Step Back One Frame", "Playback", "Left"},
    ActionInfo{"step-forward", "Step Forward One Frame", "Playback", "Right"},
    ActionInfo{"go-to-start", "Go to Start", "Playback", "Home"},
    ActionInfo{"go-to-end", "Go to End", "Playback", "End"},
    ActionInfo{"mark-in", "Mark In", "Playback", "I"},
    ActionInfo{"mark-out", "Mark Out", "Playback", "O"},
    ActionInfo{"source-back", "Source Back One Frame", "Playback", "Up"},
    ActionInfo{"source-forward", "Source Forward One Frame", "Playback", "Down"},
};

}  // namespace

std::span<const ActionInfo> allActions() {
    return kActions;
}

const ActionInfo* findAction(std::string_view id) {
    const auto found = std::find_if(kActions.begin(), kActions.end(),
                                    [id](const ActionInfo& action) { return action.id == id; });
    return found == kActions.end() ? nullptr : &*found;
}

}  // namespace zaro::ui
