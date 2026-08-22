#include "zaro/core/model/MediaSearch.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>

namespace zaro::model {
namespace {

void appendLower(std::string& into, const std::string& text) {
    for (const char letter : text) {
        into.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(letter))));
    }
    into.push_back(' ');
}

}  // namespace

std::string searchTextFor(const MediaRef& media) {
    std::string text;
    appendLower(text, media.name);
    appendLower(text, media.path);
    appendLower(text, media.notes);

    if (const media::VideoStreamInfo* video = media.info.primaryVideo()) {
        appendLower(text, video->codecName);
        // Both "1920x1080" and "1080" are things people type, and only the
        // first of them is in the string already.
        std::ostringstream size;
        size << video->width << "x" << video->height;
        appendLower(text, size.str());
        appendLower(text, std::to_string(video->height));
        std::ostringstream rate;
        rate.precision(3);
        rate << std::fixed << video->frameRate.toDouble();
        std::string rateText = rate.str();
        // 25.000 is not what anybody types; 25 is.
        while (!rateText.empty() && (rateText.back() == '0' || rateText.back() == '.')) {
            const bool wasDot = rateText.back() == '.';
            rateText.pop_back();
            if (wasDot) {
                break;
            }
        }
        appendLower(text, rateText + "fps");
        appendLower(text, rateText);
    }
    if (const media::AudioStreamInfo* audio = media.info.primaryAudio()) {
        appendLower(text, audio->codecName);
        appendLower(text, std::to_string(audio->channelCount) + "ch");
    }
    if (media.info.duration.isPositive()) {
        appendLower(text, std::to_string(static_cast<int>(media.info.duration.toDouble())) + "s");
    }
    if (media.transferOverride != media::TransferFunction::Unknown) {
        appendLower(text, media::toString(media.transferOverride));
    }
    if (!media.proxyPath.empty()) {
        // A word for the state, so "proxy" finds what has one.
        appendLower(text, "proxy");
    }
    return text;
}

bool matchesSearch(const MediaRef& media, const std::string& query) {
    const std::string haystack = searchTextFor(media);
    std::istringstream words{query};
    std::string word;
    bool sawAny = false;
    while (words >> word) {
        sawAny = true;
        std::string lowered;
        appendLower(lowered, word);
        lowered.pop_back();  // the trailing space appendLower adds
        if (haystack.find(lowered) == std::string::npos) {
            return false;
        }
    }
    static_cast<void>(sawAny);
    return true;
}

}  // namespace zaro::model
