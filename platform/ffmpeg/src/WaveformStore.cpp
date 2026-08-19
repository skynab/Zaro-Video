#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>

#include "zaro/core/media/Decoder.h"
#include "zaro/platform/ffmpeg/FFmpegMedia.h"

#include "Ffi.h"

namespace zaro::platform::ffmpeg {

Result<media::Waveform> buildWaveform(const std::string& path, std::int64_t samplesPerBucket,
                                      const std::function<bool()>& keepGoing) {
    if (samplesPerBucket <= 0) {
        return Error{ErrorCode::InvalidData, "a waveform needs a positive bucket size"};
    }
    auto opened = openAudioDecoder(path);
    if (!opened) {
        return opened.error();
    }
    media::AudioDecoder& decoder = **opened;

    media::Waveform waveform{decoder.info().channelCount, samplesPerBucket,
                             decoder.outputSampleRate()};
    while (true) {
        // Polled per block rather than per file: a single long clip is the
        // common case, so checking only between files would still make quitting
        // wait for the whole thing.
        if (keepGoing && !keepGoing()) {
            return Error{ErrorCode::Cancelled, "waveform scan abandoned"};
        }
        auto block = decoder.nextBuffer();
        if (!block) {
            break;
        }
        waveform.append(*block);
    }
    waveform.finish();
    return waveform;
}

WaveformStore::WaveformStore(std::string directory) : directory_{std::move(directory)} {
    std::error_code code;
    std::filesystem::create_directories(directory_, code);
}

Result<media::Waveform> WaveformStore::get(const std::string& path, std::int64_t samplesPerBucket,
                                           const std::function<bool()>& keepGoing) {
    const auto hash = media::quickContentHash(path);
    if (!hash) {
        return hash.error();
    }

    // The bucket size is part of the key: peaks at one resolution cannot answer
    // for another without being rebuilt, and silently returning the wrong
    // resolution would draw a waveform that does not line up with its clip.
    const std::filesystem::path file = std::filesystem::path{directory_} /
                                       (*hash + "-" + std::to_string(samplesPerBucket) + ".zwf");

    if (std::ifstream in{file, std::ios::binary}) {
        std::ostringstream buffer;
        buffer << in.rdbuf();
        if (auto decoded = media::Waveform::decode(buffer.str())) {
            return decoded;
        }
        // Damaged or written by an older build. Rebuilding is cheap enough that
        // there is no reason to report it.
    }

    auto built = buildWaveform(path, samplesPerBucket, keepGoing);
    if (!built) {
        // A cancelled scan is not cached. Writing a partial waveform would make
        // a wrong answer permanent.
        return built.error();
    }

    // Write via a temporary and rename, so a crash midway cannot leave a
    // half-written file that later reads as valid.
    const std::filesystem::path temporary = file.string() + ".partial";
    if (std::ofstream out{temporary, std::ios::binary}) {
        const std::string encoded = built->encode();
        out.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
        out.close();
        std::error_code code;
        std::filesystem::rename(temporary, file, code);
        if (code) {
            std::filesystem::remove(temporary, code);
        }
    }
    return built;
}

}  // namespace zaro::platform::ffmpeg
