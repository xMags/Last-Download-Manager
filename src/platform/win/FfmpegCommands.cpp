#include "platform/win/FfmpegCommands.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>

namespace ldm::platform {

namespace {

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// The sample entry names (the part of an RFC 6381 code before the first dot) of a codec list.
std::vector<std::string> CodecNames(const std::string& codecs)
{
    std::vector<std::string> names;
    size_t start = 0;
    while (start <= codecs.size()) {
        const size_t comma = codecs.find(',', start);
        std::string code = codecs.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        code.erase(0, code.find_first_not_of(" \t"));
        code.erase(code.find_last_not_of(" \t") + 1);
        if (!code.empty()) {
            names.push_back(Lower(code.substr(0, code.find('.'))));
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return names;
}

bool HasVideo(const std::vector<core::MuxInput>& inputs)
{
    return std::any_of(inputs.begin(), inputs.end(),
                       [](const core::MuxInput& input) { return input.role == model::TrackRole::Video; });
}

void AddInput(std::vector<std::wstring>& arguments, const MuxSource& source)
{
    if (source.playlist) {
        // LDM's own playlists name LDM's own files; their names need not match FFmpeg's list of
        // media extensions, and FFmpeg reads them by their content.
        arguments.insert(arguments.end(), {L"-allowed_extensions", L"ALL", L"-allowed_segment_extensions", L"ALL",
                                           L"-extension_picky", L"0"});
    }
    arguments.insert(arguments.end(), {L"-i", source.file.native()});
}

std::vector<std::wstring> CommonArguments()
{
    return {L"-hide_banner", L"-nostdin", L"-loglevel", L"error", L"-y", L"-protocol_whitelist",
            std::wstring(kFfmpegProtocols)};
}

std::string CodecFamily(const std::string& name)
{
    static const std::map<std::string, std::string> kNames = {
        {"h264", "H.264"}, {"hevc", "H.265"},   {"av1", "AV1"},     {"vp9", "VP9"},     {"vp8", "VP8"},
        {"mpeg4", "MPEG-4"}, {"aac", "AAC"},    {"mp3", "MP3"},     {"ac3", "AC-3"},    {"eac3", "E-AC-3"},
        {"opus", "Opus"},  {"vorbis", "Vorbis"}, {"flac", "FLAC"},  {"webvtt", "WebVTT"}, {"mpeg2video", "MPEG-2"},
    };
    const auto it = kNames.find(name);
    return it != kNames.end() ? it->second : name;
}

std::string Channels(int channels)
{
    switch (channels) {
    case 1:
        return "mono";
    case 2:
        return "stereo";
    case 6:
        return "5.1";
    case 8:
        return "7.1";
    default:
        return std::to_string(channels) + " channels";
    }
}

std::optional<double> Rate(const std::string& fraction)
{
    const size_t slash = fraction.find('/');
    try {
        if (slash == std::string::npos) {
            return std::stod(fraction);
        }
        const double numerator = std::stod(fraction.substr(0, slash));
        const double denominator = std::stod(fraction.substr(slash + 1));
        if (denominator <= 0) {
            return std::nullopt;
        }
        return numerator / denominator;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string Length(double seconds)
{
    const auto total = static_cast<long long>(std::llround(seconds));
    const long long hours = total / 3600;
    const long long minutes = (total / 60) % 60;
    const long long rest = total % 60;
    char text[32] = {};
    if (hours > 0) {
        std::snprintf(text, sizeof(text), "%lld:%02lld:%02lld", hours, minutes, rest);
    } else {
        std::snprintf(text, sizeof(text), "%lld:%02lld", minutes, rest);
    }
    return text;
}

}  // namespace

MediaContainer ContainerFor(const std::vector<core::MuxInput>& inputs)
{
    for (const core::MuxInput& input : inputs) {
        for (const std::string& name : CodecNames(input.codecs)) {
            if (name == "vp8" || name == "vorbis" || name == "theora") {
                return MediaContainer::Matroska;
            }
        }
    }
    return MediaContainer::Mp4;
}

std::string ExtensionFor(MediaContainer container, const std::vector<core::MuxInput>& inputs)
{
    const bool video = HasVideo(inputs);
    if (container == MediaContainer::Mp4) {
        return video ? "mp4" : "m4a";
    }
    return video ? "mkv" : "mka";
}

std::optional<std::string> LocalPlaylist(const core::MuxInput& input, const std::filesystem::path& folder)
{
    const auto relative = [&folder](const std::filesystem::path& file) -> std::optional<std::string> {
        std::error_code error;
        const std::filesystem::path inside = std::filesystem::relative(file, folder, error);
        if (error || inside.empty() || inside.native().starts_with(L"..") || inside.is_absolute()) {
            return std::nullopt;
        }
        const std::wstring text = inside.generic_wstring();
        if (std::any_of(text.begin(), text.end(), [](wchar_t c) { return c > 0x7E || c < 0x20 || c == L'"'; })) {
            return std::nullopt;  // LDM names its pieces in ASCII; anything else is not one of them
        }
        std::string ascii(text.size(), ' ');
        std::transform(text.begin(), text.end(), ascii.begin(), [](wchar_t c) { return static_cast<char>(c); });
        return ascii;  // every character was checked to be printable ASCII
    };
    std::int64_t longestUs = 0;
    for (const core::MuxPiece& piece : input.pieces) {
        longestUs = std::max(longestUs, piece.durationUs);
    }
    std::string text = "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-PLAYLIST-TYPE:VOD\n";
    text += "#EXT-X-TARGETDURATION:" + std::to_string(std::max<std::int64_t>(1, (longestUs + 999'999) / 1'000'000)) + "\n";
    text += "#EXT-X-MEDIA-SEQUENCE:0\n";
    std::optional<std::uint32_t> currentInit;
    for (const core::MuxPiece& piece : input.pieces) {
        if (piece.init) {
            continue;
        }
        if (piece.discontinuity) {
            text += "#EXT-X-DISCONTINUITY\n";
        }
        if (piece.initPiece != currentInit) {
            currentInit = piece.initPiece;
            if (currentInit) {
                if (*currentInit >= input.pieces.size()) {
                    return std::nullopt;
                }
                const auto map = relative(input.pieces[*currentInit].file);
                if (!map) {
                    return std::nullopt;
                }
                text += "#EXT-X-MAP:URI=\"" + *map + "\"\n";
            }
        }
        const auto file = relative(piece.file);
        if (!file) {
            return std::nullopt;
        }
        char duration[32] = {};
        // A segment without a stated length still needs one; FFmpeg reads the real one.
        std::snprintf(duration, sizeof(duration), "%.6f", static_cast<double>(std::max<std::int64_t>(piece.durationUs, 1000)) / 1e6);
        text += "#EXTINF:" + std::string(duration) + ",\n" + *file + "\n";
    }
    text += "#EXT-X-ENDLIST\n";
    return text;
}

std::vector<std::wstring> JoinArguments(const std::vector<MuxSource>& sources, MediaContainer container,
                                        const std::filesystem::path& output)
{
    std::vector<std::wstring> arguments = CommonArguments();
    for (const MuxSource& source : sources) {
        AddInput(arguments, source);
    }
    for (size_t i = 0; i < sources.size(); ++i) {
        const std::wstring n = std::to_wstring(i);
        if (sources[i].role == model::TrackRole::Video) {
            // A video track may carry its own sound; "?" lets a missing stream pass.
            arguments.insert(arguments.end(), {L"-map", n + L":v?", L"-map", n + L":a?"});
        } else if (sources[i].role == model::TrackRole::Audio) {
            arguments.insert(arguments.end(), {L"-map", n + L":a?"});
        }
    }
    arguments.insert(arguments.end(), {L"-c", L"copy", L"-dn", L"-f", container == MediaContainer::Mp4 ? L"mp4" : L"matroska",
                                       output.native()});
    return arguments;
}

std::vector<std::wstring> SubtitleArguments(const MuxSource& source, const std::filesystem::path& output)
{
    std::vector<std::wstring> arguments = CommonArguments();
    AddInput(arguments, source);
    arguments.insert(arguments.end(), {L"-map", L"0:s:0", L"-c:s", L"webvtt", L"-f", L"webvtt", output.native()});
    return arguments;
}

std::vector<std::wstring> ProbeArguments(const std::filesystem::path& file)
{
    return {L"-v",
            L"error",
            L"-protocol_whitelist",
            std::wstring(kFfmpegProtocols),
            L"-show_entries",
            L"format=duration:stream=codec_type,codec_name,width,height,avg_frame_rate,channels,sample_rate",
            L"-of",
            L"json",
            file.native()};
}

namespace {

std::string DescribeParsedProbe(std::string_view json)
{
    const auto parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return {};
    }
    std::vector<std::string> parts;
    const auto streams = parsed.value("streams", nlohmann::json::array());
    if (!streams.is_array()) {
        return {};
    }
    for (const char* type : {"video", "audio"}) {
        for (const auto& stream : streams) {
            if (!stream.is_object() || stream.value("codec_type", std::string()) != type) {
                continue;
            }
            std::vector<std::string> words;
            if (std::string_view(type) == "video") {
                const int width = stream.value("width", 0);
                const int height = stream.value("height", 0);
                if (width > 0 && height > 0) {
                    words.push_back(std::to_string(width) + "x" + std::to_string(height));
                }
                if (const auto rate = Rate(stream.value("avg_frame_rate", std::string())); rate && *rate > 0 && *rate < 1000) {
                    char text[16] = {};
                    const double rounded = std::round(*rate * 100) / 100;
                    std::snprintf(text, sizeof(text), rounded == std::floor(rounded) ? "%.0f fps" : "%.2f fps", rounded);
                    words.push_back(text);
                }
                words.push_back(CodecFamily(stream.value("codec_name", std::string())));
            } else {
                words.push_back(CodecFamily(stream.value("codec_name", std::string())));
                if (const int channels = stream.value("channels", 0); channels > 0) {
                    words.push_back(Channels(channels));
                }
                try {
                    const int rate = std::stoi(stream.value("sample_rate", std::string("0")));
                    if (rate > 0) {
                        words.push_back(rate % 1000 == 0 ? std::to_string(rate / 1000) + " kHz"
                                                         : std::to_string(rate / 1000) + "." +
                                                               std::to_string(rate % 1000 / 100) + " kHz");
                    }
                } catch (const std::exception&) {
                }
            }
            words.erase(std::remove(words.begin(), words.end(), std::string()), words.end());
            std::string joined;
            for (const std::string& word : words) {
                joined += (joined.empty() ? "" : ", ") + word;
            }
            if (!joined.empty()) {
                parts.push_back(joined);
            }
            break;  // the first stream of each kind describes the file
        }
    }
    try {
        const auto format = parsed.value("format", nlohmann::json::object());
        const double seconds = std::stod(format.value("duration", std::string("0")));
        if (seconds > 0) {
            parts.push_back(Length(seconds));
        }
    } catch (const std::exception&) {
    }
    std::string description;
    for (const std::string& part : parts) {
        description += (description.empty() ? "" : "; ") + part;
    }
    return description;
}

}  // namespace

std::string DescribeProbe(std::string_view json)
{
    try {
        return DescribeParsedProbe(json);
    } catch (const nlohmann::json::exception&) {
        return {};  // a field of an unexpected type: the description is optional
    }
}

}  // namespace ldm::platform
