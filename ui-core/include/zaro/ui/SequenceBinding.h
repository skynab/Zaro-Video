// What a panel needs to know about the thing being edited.
//
// Every panel in the application answers questions about one sequence and
// writes its changes through one command stack, so every panel held the same
// three members and took the same three arguments -- with just enough variation
// between them (some without the stack, one wanting the project const) that no
// two signatures matched. Rebinding them when the sequence changed was
// therefore a list of calls written by hand, and it named four of the eleven.
// The other seven were bound on the way into the workspace that shows them,
// which is the right place to do it first and the wrong place for it to be the
// only place: somebody already looking at the mixer when the sequence changes
// never leaves and comes back.
//
// One type, one method, one list.
#pragma once

#include "zaro/core/edit/CommandStack.h"
#include "zaro/core/model/Ids.h"
#include "zaro/core/model/Project.h"

namespace zaro::ui {

/// The project, which sequence in it, and where edits go.
///
/// Held by value: it is three pointers' worth of nothing, and a panel that
/// copies it cannot end up with a project from one binding and a sequence id
/// from another.
struct SequenceBinding {
    model::Project* project{nullptr};
    model::SequenceId sequence;
    edit::CommandStack* commands{nullptr};

    /// The sequence itself, or null.
    ///
    /// By id rather than by pointer, looked up on each call: a pointer into the
    /// project's vector of sequences stays good only until one is added, and
    /// adding one is an ordinary thing to do.
    [[nodiscard]] const model::Sequence* sequenceOrNull() const {
        return project != nullptr ? project->findSequence(sequence) : nullptr;
    }

    [[nodiscard]] model::Sequence* sequenceOrNullMutable() const {
        return project != nullptr ? project->findSequence(sequence) : nullptr;
    }

    [[nodiscard]] bool isBound() const noexcept {
        return project != nullptr && sequenceOrNull() != nullptr;
    }
};

/// A panel that is about a sequence.
///
/// Inherited alongside QWidget, which must come first: Qt requires the QObject
/// base to be the first one. The point of the interface is that rebinding is a
/// loop over a list rather than a sequence of calls somebody has to remember to
/// extend, and that a new panel cannot be added without saying how it binds.
class SequenceBound {
public:
    SequenceBound() = default;
    SequenceBound(const SequenceBound&) = delete;
    SequenceBound& operator=(const SequenceBound&) = delete;
    SequenceBound(SequenceBound&&) = delete;
    SequenceBound& operator=(SequenceBound&&) = delete;
    virtual ~SequenceBound() = default;

    /// Point this panel at a sequence, and show what it says.
    ///
    /// Called whenever any of the three change -- a different sequence, a
    /// different project, or the same project reloaded. Implementations should
    /// assume nothing they cached is still valid.
    virtual void bind(const SequenceBinding& binding) = 0;
};

}  // namespace zaro::ui
