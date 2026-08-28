#include "zaro/core/io/SubtitleIo.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <vector>

namespace zaro::io {
namespace {

/// Captions are timed in milliseconds. See model/Caption.h.
const time::Rational kMilliseconds{1000, 1};

std::string trim(const std::string& line) {
    const std::size_t first = line.find_first_not_of(" \t\r\n\f\v");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = line.find_last_not_of(" \t\r\n\f\v");
    return line.substr(first, last - first + 1);
}

/// Parse `hh:mm:ss,mmm` or `mm:ss.mmm`, with either separator.
///
/// Hours are optional in WebVTT and the separator differs between the formats,
/// so being strict about either would reject files that every other tool reads.
bool parseTimestamp(const std::string& text, std::int64_t& milliseconds) {
    std::vector<std::string> parts;
    std::string current;
    std::int64_t fraction = 0;
    bool haveFraction = false;

    for (const char c : text) {
        if (c == ':') {
            parts.push_back(current);
            current.clear();
        } else if (c == ',' || c == '.') {
            parts.push_back(current);
            current.clear();
            haveFraction = true;
        } else if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
            current.push_back(c);
        } else {
            return false;
        }
    }
    if (haveFraction) {
        // Pad or truncate to three digits: "500" and "5" are not the same
        // number of milliseconds, and files in the wild write both.
        std::string digits = current;
        digits.resize(3, '0');
        fraction = std::stoll(digits);
    } else {
        parts.push_back(current);
    }
    if (parts.empty() || parts.size() > 3) {
        return false;
    }
    for (const std::string& part : parts) {
        if (part.empty()) {
            return false;
        }
    }

    std::int64_t seconds = 0;
    for (const std::string& part : parts) {
        seconds = (seconds * 60) + std::stoll(part);
    }
    milliseconds = (seconds * 1000) + fraction;
    return true;
}

std::string formatTimestamp(std::int64_t milliseconds, SubtitleFormat format) {
    const std::int64_t clamped = std::max<std::int64_t>(0, milliseconds);
    const std::int64_t hours = clamped / 3600000;
    const std::int64_t minutes = (clamped / 60000) % 60;
    const std::int64_t seconds = (clamped / 1000) % 60;
    const std::int64_t rest = clamped % 1000;

    std::ostringstream out;
    out.fill('0');
    out.width(2);
    out << hours << ':';
    out.width(2);
    out << minutes << ':';
    out.width(2);
    out << seconds << (format == SubtitleFormat::WebVtt ? '.' : ',');
    out.width(3);
    out << rest;
    return out.str();
}

}  // namespace

Result<model::CaptionTrack> parseSubtitles(const std::string& text) {
    model::CaptionTrack track;

    std::istringstream lines{text};
    std::string raw;
    std::vector<std::string> pending;
    bool sawArrow = false;
    std::int64_t startMs = 0;
    std::int64_t endMs = 0;
    std::int32_t cueCount = 0;

    const auto flush = [&]() {
        if (!sawArrow) {
            pending.clear();
            return;
        }
        std::string body;
        for (std::size_t i = 0; i < pending.size(); ++i) {
            if (i > 0) {
                body += '\n';
            }
            body += pending[i];
        }
        model::Caption caption;
        caption.range = time::TimeRange::fromStartEnd(time::RationalTime{startMs, kMilliseconds},
                                                      time::RationalTime{endMs, kMilliseconds});
        caption.text = body;
        // Empty cues are dropped rather than kept: they show nothing, and a
        // caption list full of blanks is harder to work with than one without
        // them.
        if (!body.empty()) {
            track.add(caption);
            ++cueCount;
        }
        pending.clear();
        sawArrow = false;
    };

    while (std::getline(lines, raw)) {
        // A UTF-8 byte order mark on the first line is common and is not part
        // of the first cue's number.
        if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF &&
            static_cast<unsigned char>(raw[1]) == 0xBB &&
            static_cast<unsigned char>(raw[2]) == 0xBF) {
            raw = raw.substr(3);
        }
        const std::string line = trim(raw);

        if (line.empty()) {
            flush();
            continue;
        }
        if (line == "WEBVTT" || line.rfind("WEBVTT", 0) == 0) {
            continue;
        }

        const std::size_t arrow = line.find("-->");
        if (arrow != std::string::npos) {
            const std::string from = trim(line.substr(0, arrow));
            // Anything after the timestamps is a WebVTT cue setting, which this
            // reader does not use but must not choke on.
            std::string to = trim(line.substr(arrow + 3));
            if (const std::size_t space = to.find_first_of(" \t"); space != std::string::npos) {
                to = to.substr(0, space);
            }
            if (!parseTimestamp(from, startMs) || !parseTimestamp(to, endMs)) {
                return Error{ErrorCode::InvalidData, "cannot read the timestamps in: " + line};
            }
            if (endMs < startMs) {
                return Error{ErrorCode::InvalidData, "a caption ends before it starts: " + line};
            }
            sawArrow = true;
            // A cue number on the line before is not part of the text.
            pending.clear();
            continue;
        }
        pending.push_back(line);
    }
    flush();

    if (cueCount == 0) {
        return Error{ErrorCode::InvalidData, "the file contains no captions"};
    }
    return track;
}

Result<model::CaptionTrack> loadSubtitles(const std::string& path) {
    std::ifstream file{path};
    if (!file) {
        return Error{ErrorCode::NotFound, "cannot open " + path};
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return parseSubtitles(buffer.str());
}

std::string writeSubtitles(const model::CaptionTrack& captions, SubtitleFormat format) {
    std::ostringstream out;
    if (format == SubtitleFormat::WebVtt) {
        out << "WEBVTT\n\n";
    }
    std::int32_t number = 0;
    for (const model::Caption& caption : captions.captions()) {
        ++number;
        const auto start = caption.range.start().rescaledTo(kMilliseconds).frames();
        const auto end = caption.range.endExclusive().rescaledTo(kMilliseconds).frames();
        // SubRip numbers its cues; WebVTT allows an identifier and does not
        // need one, so it gets none rather than a number that means nothing.
        if (format == SubtitleFormat::SubRip) {
            out << number << '\n';
        }
        out << formatTimestamp(start, format) << " --> " << formatTimestamp(end, format) << '\n';
        out << caption.text << "\n\n";
    }
    return out.str();
}

Status saveSubtitles(const model::CaptionTrack& captions, const std::string& path,
                     SubtitleFormat format) {
    std::ofstream file{path, std::ios::binary};
    if (!file) {
        return Error{ErrorCode::Io, "cannot write " + path};
    }
    const std::string text = writeSubtitles(captions, format);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file) {
        return Error{ErrorCode::Io, "cannot write " + path};
    }
    return {};
}

SubtitleFormat formatForPath(const std::string& path) {
    const std::size_t dot = path.rfind('.');
    if (dot == std::string::npos) {
        return SubtitleFormat::SubRip;
    }
    std::string extension = path.substr(dot + 1);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == "vtt" ? SubtitleFormat::WebVtt : SubtitleFormat::SubRip;
}

}  // namespace zaro::io
