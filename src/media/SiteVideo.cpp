#include "media/SiteVideo.h"

#include "media/UrlReference.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

namespace ldm::media {

namespace {

using Json = nlohmann::json;

base::Error Refused(std::string message)
{
    return base::MakeError(base::ErrorCode::Unsupported, std::move(message));
}

base::Error Unreadable()
{
    return base::MakeError(base::ErrorCode::Protocol, "The video site's answer could not be read.");
}

std::string Text(const Json& object, const char* key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::optional<double> Number(const Json& object, const char* key)
{
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number()) {
        return std::nullopt;
    }
    const double value = it->get<double>();
    return std::isfinite(value) && value > 0 ? std::optional<double>(value) : std::nullopt;
}

// A byte count: a positive whole number that fits in an int64.
std::optional<std::int64_t> Bytes(const Json& object, const char* key)
{
    const auto it = object.find(key);
    if (it == object.end()) {
        return std::nullopt;
    }
    if (it->is_number_unsigned()) {
        const auto value = it->get<std::uint64_t>();
        return value > 0 && value <= static_cast<std::uint64_t>(INT64_MAX) ? std::optional<std::int64_t>(static_cast<std::int64_t>(value))
                                                                         : std::nullopt;
    }
    if (it->is_number_integer()) {
        const auto value = it->get<std::int64_t>();
        return value > 0 ? std::optional<std::int64_t>(value) : std::nullopt;
    }
    return std::nullopt;
}

int Whole(const Json& object, const char* key)
{
    const auto value = Number(object, key);
    return value && *value < 1'000'000 ? static_cast<int>(*value) : 0;
}

bool Flag(const Json& object, const char* key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_boolean() && it->get<bool>();
}

// The codecs LDM takes from a site. yt-dlp says "none" for a stream a format does not have and
// leaves the field out (or null) when it does not know; an unknown codec is never guessed at.
enum class Codec {
    None,
    Unknown,
    H264,
    Vp9,
    Av1,
    Aac,
    Opus,
};

Codec VideoCodec(std::string_view codec)
{
    if (codec == "none") {
        return Codec::None;
    }
    if (codec.starts_with("avc1") || codec.starts_with("avc3")) {
        return Codec::H264;
    }
    if (codec.starts_with("vp09") || codec.starts_with("vp9")) {
        return Codec::Vp9;
    }
    if (codec.starts_with("av01")) {
        return Codec::Av1;
    }
    return Codec::Unknown;
}

Codec AudioCodec(std::string_view codec)
{
    if (codec == "none") {
        return Codec::None;
    }
    if (codec.starts_with("mp4a.40.") && codec != "mp4a.40.34") {  // .34 is MP3 in an MP4 box
        return Codec::Aac;
    }
    if (codec == "opus") {
        return Codec::Opus;
    }
    return Codec::Unknown;
}

// Among formats of the same picture: H.264 plays everywhere, VP9 and AV1 need newer players.
int VideoRank(Codec codec)
{
    return codec == Codec::H264 ? 0 : codec == Codec::Vp9 ? 1 : 2;
}

// One format that passed the checks every kind needs.
struct Candidate {
    std::string id;
    std::string url;
    std::string ext;
    std::string vcodec;
    std::string acodec;
    Codec video = Codec::None;
    Codec audio = Codec::None;
    int width = 0;
    int height = 0;
    double fps = 0;
    double kbps = 0;  // yt-dlp's tbr
    std::optional<std::int64_t> bytes;  // exact
    std::string language;
    int languagePreference = 0;
    std::string note;
    int channels = 0;  // 0 when not stated
    bool sdr = true;
};

// A format LDM can fetch as one file: a direct http(s) address, no DRM, codecs it knows.
std::optional<Candidate> ReadCandidate(const Json& format, bool& protectedSeen)
{
    if (!format.is_object()) {
        return std::nullopt;
    }
    if (Flag(format, "has_drm")) {
        protectedSeen = true;
        return std::nullopt;
    }
    const std::string protocol = Text(format, "protocol");
    if (protocol != "https" && protocol != "http") {
        return std::nullopt;  // HLS and DASH pieces, storyboards, and anything newer
    }
    Candidate candidate;
    candidate.id = Text(format, "format_id");
    if (candidate.id.empty() || candidate.id.size() > 64) {
        return std::nullopt;
    }
    const std::string url = Text(format, "url");
    if (url.empty() || url.size() > kMaxSiteUrlBytes) {
        return std::nullopt;
    }
    const auto checked = ResolveMediaUrl(url, url);
    if (!checked) {
        return std::nullopt;
    }
    candidate.url = *checked;
    candidate.ext = Text(format, "ext");
    candidate.vcodec = Text(format, "vcodec");
    candidate.acodec = Text(format, "acodec");
    candidate.video = VideoCodec(candidate.vcodec);
    candidate.audio = AudioCodec(candidate.acodec);
    if (candidate.video == Codec::Unknown || candidate.audio == Codec::Unknown ||
        (candidate.video == Codec::None && candidate.audio == Codec::None)) {
        return std::nullopt;
    }
    candidate.width = Whole(format, "width");
    candidate.height = Whole(format, "height");
    if (candidate.video != Codec::None && candidate.height == 0) {
        return std::nullopt;  // a picture of unknown size cannot be listed sensibly
    }
    candidate.fps = Number(format, "fps").value_or(0);
    candidate.kbps = Number(format, "tbr").value_or(0);
    candidate.bytes = Bytes(format, "filesize");
    candidate.language = Text(format, "language");
    if (candidate.language.size() > 35) {  // longer than any BCP 47 tag in use
        candidate.language.clear();
    }
    if (const auto it = format.find("language_preference"); it != format.end() && it->is_number_integer()) {
        candidate.languagePreference = static_cast<int>(std::clamp<std::int64_t>(it->get<std::int64_t>(), -1000, 1000));
    }
    candidate.note = Text(format, "format_note");
    if (candidate.note.size() > 128) {
        candidate.note.clear();
    }
    candidate.channels = Whole(format, "audio_channels");
    const std::string range = Text(format, "dynamic_range");
    candidate.sdr = range.empty() || range == "SDR";
    return candidate;
}

// A dynamic-range-compressed copy of an audio format (YouTube's "-drc" ids): quieter parts made
// louder. The plain one is what the video was published with.
bool IsCompressedAudio(const Candidate& candidate)
{
    return candidate.id.ends_with("-drc") || candidate.note.find("DRC") != std::string::npos;
}

// Frame rates up to 31 count as one class, so 24, 25 and 30 fps do not make three entries.
int FrameRateClass(double fps)
{
    return fps > 31 ? static_cast<int>(std::lround(fps)) : 0;
}

// Whether `a` is the better pick than `b` for the same picture.
bool BetterVideo(const Candidate& a, const Candidate& b)
{
    if (a.sdr != b.sdr) {
        return a.sdr;
    }
    if (VideoRank(a.video) != VideoRank(b.video)) {
        return VideoRank(a.video) < VideoRank(b.video);
    }
    return a.kbps > b.kbps;
}

// Bits per second as the quality list uses them: from the exact length when the site states one,
// so the list's size estimate is the real size; otherwise the site's own figure.
std::uint64_t Bandwidth(const Candidate& candidate, std::optional<double> seconds)
{
    if (candidate.bytes && seconds) {
        return static_cast<std::uint64_t>(static_cast<double>(*candidate.bytes) * 8 / *seconds);
    }
    return static_cast<std::uint64_t>(candidate.kbps * 1000);
}

SegmentSource WholeFile(const Candidate& candidate, std::optional<double> seconds)
{
    TrackSegments track;
    Segment segment;
    segment.url = candidate.url;
    segment.seconds = seconds.value_or(0);
    track.targetSeconds = segment.seconds;
    track.segments.push_back(std::move(segment));
    return track;
}

std::string Container(const Candidate& candidate)
{
    return candidate.ext == "webm" ? "webm" : "mp4";
}

Variant MakeVariant(const Candidate& candidate, std::optional<double> seconds)
{
    Variant variant;
    variant.bandwidth = Bandwidth(candidate, seconds);
    variant.resolution = Resolution{candidate.width, candidate.height};
    if (candidate.fps > 0) {
        variant.frameRate = candidate.fps;
    }
    variant.codecs = candidate.audio == Codec::None ? candidate.vcodec : candidate.vcodec + "," + candidate.acodec;
    variant.mimeType = "video/" + Container(candidate);
    variant.source = WholeFile(candidate, seconds);
    variant.site = SiteFormat{candidate.id, candidate.bytes};
    return variant;
}

Rendition MakeAudio(const Candidate& candidate, std::optional<double> seconds)
{
    Rendition rendition;
    rendition.groupId = "audio";
    rendition.name = candidate.note;
    rendition.language = candidate.language;
    rendition.bandwidth = Bandwidth(candidate, seconds);
    rendition.codecs = candidate.acodec;
    rendition.mimeType = "audio/" + Container(candidate);
    rendition.source = WholeFile(candidate, seconds);
    rendition.site = SiteFormat{candidate.id, candidate.bytes};
    return rendition;
}

}  // namespace

base::Result<SiteVideo> ParseSiteVideo(std::string_view json)
{
    if (json.size() > kMaxSiteAnswerBytes) {
        return Refused("The video site's answer is too large.");
    }
    const Json root = Json::parse(json.begin(), json.end(), nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return Unreadable();
    }
    if (const std::string type = Text(root, "_type"); !type.empty() && type != "video") {
        return Refused(type == "playlist" ? "This page is a playlist. Open one of its videos to download it."
                                          : "This page is not a single video.");
    }
    const std::string liveStatus = Text(root, "live_status");
    if (liveStatus == "is_upcoming") {
        return Refused("This video has not started yet.");
    }
    if (Flag(root, "is_live") || liveStatus == "is_live") {
        return Refused("Live streams from this site cannot be downloaded yet.");
    }

    SiteVideo video;
    video.id = Text(root, "id");
    if (video.id.empty() || video.id.size() > 256) {
        return Unreadable();
    }
    video.title = Text(root, "title");
    if (video.title.size() > 4096) {
        video.title.clear();
    }
    if (const auto seconds = Number(root, "duration"); seconds && *seconds < 10'000'000) {
        video.seconds = seconds;
    }

    const auto formats = root.find("formats");
    if (formats == root.end() || !formats->is_array()) {
        return Unreadable();
    }
    bool protectedSeen = false;
    std::vector<Candidate> videoOnly;
    std::vector<Candidate> combined;
    std::vector<Candidate> audio;
    const size_t count = std::min(formats->size(), kMaxSiteFormats);
    for (size_t i = 0; i < count; ++i) {
        auto candidate = ReadCandidate((*formats)[i], protectedSeen);
        if (!candidate) {
            continue;
        }
        if (candidate->audio == Codec::None) {
            videoOnly.push_back(std::move(*candidate));
        } else if (candidate->video == Codec::None) {
            audio.push_back(std::move(*candidate));
        } else {
            combined.push_back(std::move(*candidate));
        }
    }

    // One picture per height and frame rate class. With separate pictures available, formats
    // that carry their own sound are left out: on these sites they are the low qualities kept
    // for old players. Without them, those are the qualities, each with its own sound.
    const bool separate = !videoOnly.empty() && !audio.empty();
    std::vector<Candidate>& pictures = separate ? videoOnly : combined;
    std::map<std::pair<int, int>, size_t> bestByPicture;
    for (size_t i = 0; i < pictures.size(); ++i) {
        const auto key = std::make_pair(pictures[i].height, FrameRateClass(pictures[i].fps));
        const auto found = bestByPicture.find(key);
        if (found == bestByPicture.end()) {
            bestByPicture.emplace(key, i);
        } else if (BetterVideo(pictures[i], pictures[found->second])) {
            found->second = i;
        }
    }

    // The best stereo track per language and codec; surround and compressed copies are left out.
    std::map<std::pair<std::string, Codec>, size_t> bestAudio;
    for (size_t i = 0; i < audio.size(); ++i) {
        if (IsCompressedAudio(audio[i]) || audio[i].channels > 2) {
            continue;
        }
        const auto key = std::make_pair(audio[i].language, audio[i].audio);
        const auto found = bestAudio.find(key);
        if (found == bestAudio.end() || audio[i].kbps > audio[found->second].kbps) {
            bestAudio[key] = i;
        }
    }

    MediaOffer& offer = video.offer;
    offer.format = separate ? StreamFormat::Dash : StreamFormat::Hls;
    offer.seconds = video.seconds;
    for (const auto& [key, index] : bestByPicture) {
        offer.variants.push_back(MakeVariant(pictures[index], video.seconds));
    }
    // Highest picture first, as the quality list sorts them too; keeps the offer readable in logs.
    std::reverse(offer.variants.begin(), offer.variants.end());

    // The track paired with every picture: the site's preferred language (the original audio of a
    // dubbed video), in AAC, which MP4 players all take.
    std::optional<size_t> pairedIndex;
    for (const auto& [key, index] : bestAudio) {
        const Candidate& candidate = audio[index];
        if (!pairedIndex) {
            pairedIndex = index;
            continue;
        }
        const Candidate& current = audio[*pairedIndex];
        const auto rank = [](const Candidate& c) { return std::make_tuple(c.languagePreference, c.audio == Codec::Aac, c.kbps); };
        if (rank(candidate) > rank(current)) {
            pairedIndex = index;
        }
    }
    for (const auto& [key, index] : bestAudio) {
        Rendition rendition = MakeAudio(audio[index], video.seconds);
        rendition.isDefault = pairedIndex && index == *pairedIndex;
        if (!separate) {
            rendition.groupId.clear();  // sound-only qualities beside pictures that have their own
        }
        offer.audio.push_back(std::move(rendition));
    }

    if (offer.variants.empty() && offer.audio.empty()) {
        return Refused(protectedSeen ? "This video is protected and cannot be downloaded."
                                     : "This video has no format LDM can download.");
    }
    return video;
}

}  // namespace ldm::media
