#include "Transcript.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cctype>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/time/Timecode.h"

namespace zaro::app {
namespace {

/// The filler words offered by the button.
///
/// A list, not a language model: these are the ones that are filler in almost
/// every sentence they appear in. "Like" and "so" are not here on purpose --
/// they are ordinary words far more often than they are filler, and a tool that
/// cut them would be cutting speech.
const std::vector<std::string>& fillerWords() {
    static const std::vector<std::string> words{"um", "uh", "erm", "er", "ah", "hmm", "mm"};
    return words;
}

[[nodiscard]] std::string lowered(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char letter : text) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(letter))));
    }
    return out;
}

/// Whether a line contains one of these words as a *word*.
///
/// Not a substring test: "um" is in "number", "column" and "umbrella", and a
/// filler remover that cut those would be a tool nobody could trust with a
/// transcript.
[[nodiscard]] bool mentions(const std::string& text, const std::vector<std::string>& words) {
    const std::string haystack = lowered(text);
    for (const std::string& word : words) {
        const std::string needle = lowered(word);
        std::size_t at = haystack.find(needle);
        while (at != std::string::npos) {
            const bool startsWord =
                at == 0 || std::isalnum(static_cast<unsigned char>(haystack[at - 1])) == 0;
            const std::size_t after = at + needle.size();
            const bool endsWord = after >= haystack.size() ||
                                  std::isalnum(static_cast<unsigned char>(haystack[after])) == 0;
            if (startsWord && endsWord) {
                return true;
            }
            at = haystack.find(needle, at + 1);
        }
    }
    return false;
}

}  // namespace

Transcript::Transcript(QWidget* parent) : QDialog{parent} {
    setWindowTitle("Transcript");
    resize(620, 460);

    list_ = new QListWidget(this);
    list_->setObjectName("transcript-list");
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);

    auto* deleteButton = new QPushButton("Delete Selected", this);
    deleteButton->setObjectName("transcript-delete");
    deleteButton->setToolTip("Remove these lines, and the picture and sound under them");
    auto* fillerButton = new QPushButton("Select Filler", this);
    fillerButton->setObjectName("transcript-filler");
    fillerButton->setToolTip("Select the lines with um, uh and the like in them");
    auto* close = new QPushButton("Close", this);

    footer_ = new QLabel(this);
    footer_->setProperty("muted", true);

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(footer_, 1);
    buttons->addWidget(fillerButton);
    buttons->addWidget(deleteButton);
    buttons->addWidget(close);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(list_, 1);
    layout->addLayout(buttons);

    connect(list_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
        if (item == nullptr || project_ == nullptr) {
            return;
        }
        const model::Sequence* sequence = project_->findSequence(sequenceId_);
        if (sequence == nullptr) {
            return;
        }
        emit scrubbed(
            time::RationalTime{item->data(Qt::UserRole).toLongLong(), sequence->frameRate()});
    });
    connect(fillerButton, &QPushButton::clicked, this,
            [this] { static_cast<void>(selectContaining(fillerWords())); });
    connect(deleteButton, &QPushButton::clicked, this, [this] {
        if (auto gone = deleteSelected(); gone && *gone > 0) {
            emit edited();
        }
    });
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
}

void Transcript::bind(const ui::SequenceBinding& binding) {
    project_ = binding.project;
    sequenceId_ = binding.sequence;
    commands_ = binding.commands;
    refresh();
}

void Transcript::refresh() {
    list_->clear();
    const model::Sequence* sequence =
        project_ != nullptr ? project_->findSequence(sequenceId_) : nullptr;
    if (sequence == nullptr) {
        footer_->setText("No sequence");
        return;
    }
    const bool dropFrame = time::supportsDropFrame(sequence->frameRate());
    for (const model::Caption& caption : sequence->captions().captions()) {
        const time::Timecode at = time::timecodeFromTime(caption.range.start(), dropFrame);
        auto* item =
            new QListWidgetItem(QString("%1   %2").arg(QString::fromStdString(at.toString()),
                                                       QString::fromStdString(caption.text)),
                                list_);
        item->setData(Qt::UserRole, static_cast<qlonglong>(caption.range.start().frames()));
    }
    footer_->setText(sequence->captions().empty()
                         ? QString("No transcript — import captions to edit by text")
                         : QString("%1 lines").arg(list_->count()));
}

int Transcript::lineCount() const {
    return list_->count();
}

int Transcript::selectContaining(const std::vector<std::string>& words) {
    const model::Sequence* sequence =
        project_ != nullptr ? project_->findSequence(sequenceId_) : nullptr;
    if (sequence == nullptr) {
        return 0;
    }
    list_->clearSelection();
    int chosen = 0;
    const auto& captions = sequence->captions().captions();
    for (int row = 0; row < list_->count() && row < static_cast<int>(captions.size()); ++row) {
        if (mentions(captions[static_cast<std::size_t>(row)].text, words)) {
            list_->item(row)->setSelected(true);
            ++chosen;
        }
    }
    return chosen;
}

Result<int> Transcript::deleteSelected() {
    const model::Sequence* sequence =
        project_ != nullptr ? project_->findSequence(sequenceId_) : nullptr;
    if (sequence == nullptr || commands_ == nullptr) {
        return Error{ErrorCode::InvalidData, "there is no sequence to edit"};
    }
    const auto& captions = sequence->captions().captions();

    std::vector<time::TimeRange> spans;
    for (int row = 0; row < list_->count(); ++row) {
        if (!list_->item(row)->isSelected() || row >= static_cast<int>(captions.size())) {
            continue;
        }
        spans.push_back(captions[static_cast<std::size_t>(row)].range);
    }
    if (spans.empty()) {
        return Error{ErrorCode::InvalidData, "nothing is selected"};
    }

    auto built = edit::makeDeleteSpans(*project_, sequenceId_, spans);
    if (!built) {
        return built.error();
    }
    commands_->execute(*project_, std::move(*built));
    commands_->breakMerge();
    refresh();
    return static_cast<int>(spans.size());
}

}  // namespace zaro::app
