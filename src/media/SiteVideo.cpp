#include "media/SiteVideo.h"

#include "media/UrlReference.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
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

// Text that may go into a request header or a file name: bounded, without control characters.
bool PlainText(std::string_view text, size_t limit)
{
    return text.size() <= limit &&
           std::none_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7f; });
}

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; });
    return text;
}

// The codecs LDM takes from a site. yt-dlp says "none" for a stream a format does not have and
// leaves the field out (or null) when it does not know: Unstated, which the joining takes as it
// comes (MKV when MP4 cannot hold it). A codec it names that LDM does not take (AC-3, E-AC-3,
// IAMF, ...) is Unknown, and the format is left out.
enum class Codec {
    None,
    Unstated,
    Unknown,
    H264,
    H265,
    Vp8,
    Vp9,
    Av1,
    Aac,
    Opus,
    Mp3,
    Vorbis,
    Flac,
};

Codec VideoCodec(std::string_view codec)
{
    if (codec.empty()) {
        return Codec::Unstated;
    }
    if (codec == "none") {
        return Codec::None;
    }
    if (codec.starts_with("avc1") || codec.starts_with("avc3") || codec == "h264") {
        return Codec::H264;
    }
    if (codec.starts_with("hvc1") || codec.starts_with("hev1") || codec == "h265" || codec == "hevc") {
        return Codec::H265;
    }
    if (codec.starts_with("vp09") || codec.starts_with("vp9")) {
        return Codec::Vp9;
    }
    if (codec == "vp8" || codec.starts_with("vp8.")) {
        return Codec::Vp8;
    }
    if (codec.starts_with("av01") || codec == "av1") {
        return Codec::Av1;
    }
    return Codec::Unknown;
}

Codec AudioCodec(std::string_view codec)
{
    if (codec.empty()) {
        return Codec::Unstated;
    }
    if (codec == "none") {
        return Codec::None;
    }
    if (codec == "mp4a.40.34" || codec == "mp4a.6b" || codec == "mp3") {  // MP3, in an MP4 box or not
        return Codec::Mp3;
    }
    if (codec.starts_with("mp4a.40.") || codec == "mp4a" || codec == "aac") {
        return Codec::Aac;
    }
    if (codec == "opus") {
        return Codec::Opus;
    }
    if (codec == "vorbis") {
        return Codec::Vorbis;
    }
    if (codec == "flac") {
        return Codec::Flac;
    }
    return Codec::Unknown;
}

// Among formats of the same picture: H.264 plays everywhere, VP9 and AV1 need newer players,
// H.265 a paid Windows extension; one the site does not name comes last.
int VideoRank(Codec codec)
{
    switch (codec) {
    case Codec::H264:
        return 0;
    case Codec::Vp9:
        return 1;
    case Codec::Av1:
        return 2;
    case Codec::Vp8:
        return 3;
    case Codec::H265:
        return 4;
    default:
        return 5;
    }
}

// How LDM downloads a format. One quality never mixes two of them: the tracks of a download are
// all files or DASH piece lists, or all HLS playlists.
enum class Delivery {
    File,    // one http(s) file, fetched in ranges
    Pieces,  // a DASH piece list, given in full
    Hls,     // an HLS media playlist, read when the download starts
};

// Whole files resume cleanly with few requests; HLS needs its playlist read again.
int DeliveryRank(Delivery delivery)
{
    return delivery == Delivery::File ? 0 : delivery == Delivery::Pieces ? 1 : 2;
}

std::string DeliveryGroup(Delivery delivery)
{
    return delivery == Delivery::File ? "file" : delivery == Delivery::Pieces ? "pieces" : "hls";
}

// One format that passed the checks every kind needs.
struct Candidate {
    std::string id;
    Delivery delivery = Delivery::File;
    std::string url;       // File: the file; Hls: the media playlist; Pieces: unused
    TrackSegments pieces;  // Pieces only
    std::string ext;
    std::string vcodec;  // empty when not stated
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
    std::string referer;
};

std::optional<std::string> CheckedUrl(std::string_view base, std::string_view url)
{
    if (url.empty() || url.size() > kMaxSiteUrlBytes || base.size() > kMaxSiteUrlBytes) {
        return std::nullopt;
    }
    auto checked = ResolveMediaUrl(base, url);
    if (!checked || checked->size() > kMaxSiteUrlBytes) {
        return std::nullopt;
    }
    return checked;
}

// A DASH format the resolver listed piece by piece: `fragment_base_url` and `fragments`, each with
// an absolute `url` or a `path` under the base, and its `duration`. yt-dlp lists the init section
// as the first piece, without a duration; it becomes the track's init, as a DASH manifest's would.
std::optional<TrackSegments> ReadPieces(const Json& format)
{
    const auto fragments = format.find("fragments");
    if (fragments == format.end() || !fragments->is_array() || fragments->empty() ||
        fragments->size() > kMaxSegmentsPerTrack) {
        return std::nullopt;
    }
    const std::string base = Text(format, "fragment_base_url");
    std::optional<std::string> checkedBase;
    if (!base.empty()) {
        checkedBase = CheckedUrl(base, base);
        if (!checkedBase) {
            return std::nullopt;
        }
    }
    TrackSegments track;
    track.segments.reserve(fragments->size());
    for (const Json& fragment : *fragments) {
        if (!fragment.is_object() || fragment.contains("range") || fragment.contains("byte_range")) {
            return std::nullopt;  // pieces in byte ranges are not taken: never guessed at
        }
        const std::string url = Text(fragment, "url");
        const std::string path = Text(fragment, "path");
        std::optional<std::string> checked;
        if (!url.empty()) {
            checked = CheckedUrl(url, url);
        } else if (!path.empty() && checkedBase) {
            checked = CheckedUrl(*checkedBase, path);
        }
        if (!checked) {
            return std::nullopt;
        }
        const auto seconds = Number(fragment, "duration");
        if (track.inits.empty() && track.segments.empty() && !seconds && fragments->size() > 1) {
            // The init section: what every later piece needs to be read (the MP4 header).
            track.inits.push_back(InitSection{std::move(*checked), std::nullopt});
            continue;
        }
        Segment segment;
        segment.url = std::move(*checked);
        segment.seconds = seconds.value_or(0);
        segment.sequence = track.segments.size();
        if (!track.inits.empty()) {
            segment.init = 0;
        }
        track.targetSeconds = std::max(track.targetSeconds, segment.seconds);
        track.segments.push_back(std::move(segment));
    }
    return track;
}

// A format LDM can download: a direct http(s) file, an HLS media playlist or a DASH piece list,
// no DRM, codecs it takes.
std::optional<Candidate> ReadCandidate(const Json& format, bool& protectedSeen)
{
    if (!format.is_object()) {
        return std::nullopt;
    }
    if (Flag(format, "has_drm")) {
        protectedSeen = true;
        return std::nullopt;
    }
    Candidate candidate;
    const std::string protocol = Text(format, "protocol");
    if (protocol == "https" || protocol == "http") {
        candidate.delivery = Delivery::File;
    } else if (protocol == "m3u8_native" || protocol == "m3u8") {
        candidate.delivery = Delivery::Hls;
    } else if (protocol == "http_dash_segments") {
        candidate.delivery = Delivery::Pieces;
    } else {
        return std::nullopt;  // storyboards, f4m, ism, rtmp, websockets and anything newer
    }
    candidate.id = Text(format, "format_id");
    if (candidate.id.empty() || !PlainText(candidate.id, 64)) {
        return std::nullopt;
    }
    if (candidate.delivery == Delivery::Pieces) {
        auto pieces = ReadPieces(format);
        if (!pieces) {
            return std::nullopt;
        }
        candidate.pieces = std::move(*pieces);
    } else {
        const std::string url = Text(format, "url");
        auto checked = CheckedUrl(url, url);
        if (!checked) {
            return std::nullopt;
        }
        candidate.url = std::move(*checked);
    }
    candidate.ext = Text(format, "ext");
    if (candidate.ext.size() > 8 || !std::all_of(candidate.ext.begin(), candidate.ext.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        })) {
        candidate.ext.clear();
    }
    candidate.vcodec = Text(format, "vcodec");
    candidate.acodec = Text(format, "acodec");
    if (!PlainText(candidate.vcodec, 64) || !PlainText(candidate.acodec, 64)) {
        return std::nullopt;
    }
    candidate.video = VideoCodec(candidate.vcodec);
    candidate.audio = AudioCodec(candidate.acodec);
    if (candidate.video == Codec::Unknown || candidate.audio == Codec::Unknown ||
        (candidate.video == Codec::None && candidate.audio == Codec::None)) {
        return std::nullopt;
    }
    candidate.width = Whole(format, "width");
    candidate.height = Whole(format, "height");
    candidate.fps = Number(format, "fps").value_or(0);
    candidate.kbps = Number(format, "tbr").value_or(0);
    candidate.bytes = Bytes(format, "filesize");
    candidate.language = Text(format, "language");
    if (!PlainText(candidate.language, 35)) {  // longer than any BCP 47 tag in use
        candidate.language.clear();
    }
    if (const auto it = format.find("language_preference"); it != format.end() && it->is_number_integer()) {
        candidate.languagePreference = static_cast<int>(std::clamp<std::int64_t>(it->get<std::int64_t>(), -1000, 1000));
    }
    candidate.note = Text(format, "format_note");
    if (!PlainText(candidate.note, 128)) {
        candidate.note.clear();
    }
    candidate.channels = Whole(format, "audio_channels");
    const std::string range = Text(format, "dynamic_range");
    candidate.sdr = range.empty() || range == "SDR";
    if (const auto headers = format.find("http_headers"); headers != format.end() && headers->is_object()) {
        // Only the Referer: the site's files answer the browser's own User-Agent (video-sites
        // design, section 8), and the other headers yt-dlp lists are its defaults.
        const std::string referer = Text(*headers, "Referer");
        if (!referer.empty() && PlainText(referer, 2048) && CheckedUrl(referer, referer)) {
            candidate.referer = referer;
        }
    }
    return candidate;
}

bool IsCookieAttribute(const std::string& lower)
{
    return lower == "domain" || lower == "path" || lower == "expires" || lower == "max-age" || lower == "secure" ||
           lower == "httponly" || lower == "samesite" || lower == "comment" || lower == "version" || lower == "discard" ||
           lower == "port";
}

bool IsHostName(std::string_view host)
{
    return !host.empty() && host.size() <= 253 && std::all_of(host.begin(), host.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
    });
}

// The cookies of a format's `cookies` text, as yt-dlp writes it: "name=value; Domain=.site;
// Path=/; Secure; Expires=1795492351; name2=value2; ...", each cookie's attributes after it. A
// cookie without a Domain is not taken: nothing says where it goes. Text that does not read this
// way gives no cookies at all.
void ReadCookies(const Json& format, std::vector<SiteCookie>& cookies)
{
    const std::string text = Text(format, "cookies");
    if (text.empty() || text.size() > 64 * 1024) {
        return;
    }
    std::vector<SiteCookie> read;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find(';', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string part = text.substr(start, end - start);
        start = end + 1;
        const size_t first = part.find_first_not_of(' ');
        if (first == std::string::npos) {
            continue;
        }
        part.erase(0, first);
        const size_t equals = part.find('=');
        const std::string name = part.substr(0, equals);
        const std::string value = equals == std::string::npos ? std::string() : part.substr(equals + 1);
        const std::string lower = Lower(name);
        if (!IsCookieAttribute(lower)) {
            if (name.empty() || equals == std::string::npos) {
                return;
            }
            SiteCookie cookie;
            cookie.name = name;
            cookie.value = value;
            read.push_back(std::move(cookie));
            continue;
        }
        if (read.empty()) {
            return;
        }
        SiteCookie& cookie = read.back();
        if (lower == "domain") {
            cookie.hostOnly = !value.starts_with('.');
            cookie.domain = Lower(value.substr(value.starts_with('.') ? 1 : 0));
        } else if (lower == "path") {
            cookie.path = value;
        } else if (lower == "secure") {
            cookie.secure = true;
        } else if (lower == "expires") {
            const bool digits = !value.empty() && value.size() < 19 &&
                                std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; });
            cookie.expires = digits ? std::stoll(value) : 0;
        }
    }
    for (SiteCookie& cookie : read) {
        const bool ok = IsHostName(cookie.domain) && cookie.path.starts_with('/') && PlainText(cookie.path, 1024) &&
                        PlainText(cookie.name, 256) && PlainText(cookie.value, 4096) &&
                        cookie.name.find_first_of(" \t=") == std::string::npos;
        if (!ok || cookies.size() >= kMaxSiteCookies) {
            continue;
        }
        const bool known = std::any_of(cookies.begin(), cookies.end(), [&cookie](const SiteCookie& c) {
            return c.domain == cookie.domain && c.path == cookie.path && c.name == cookie.name;
        });
        if (!known) {
            cookies.push_back(std::move(cookie));
        }
    }
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

// One picture of the offer: the format, and whether its sound is a separate track.
struct Picture {
    const Candidate* candidate = nullptr;
    bool separate = false;
};

// Whether `a` is the better pick than `b` for the same picture. One with separate sound beats one
// with its own: on these sites those are the low qualities kept for old players.
bool BetterPicture(const Picture& a, const Picture& b)
{
    const Candidate& x = *a.candidate;
    const Candidate& y = *b.candidate;
    if (x.sdr != y.sdr) {
        return x.sdr;
    }
    if (a.separate != b.separate) {
        return a.separate;
    }
    if (DeliveryRank(x.delivery) != DeliveryRank(y.delivery)) {
        return DeliveryRank(x.delivery) < DeliveryRank(y.delivery);
    }
    if (VideoRank(x.video) != VideoRank(y.video)) {
        return VideoRank(x.video) < VideoRank(y.video);
    }
    return x.kbps > y.kbps;
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

SegmentSource Source(const Candidate& candidate, std::optional<double> seconds)
{
    if (candidate.delivery == Delivery::Hls) {
        return PlaylistAddress{candidate.url};
    }
    if (candidate.delivery == Delivery::Pieces) {
        return candidate.pieces;
    }
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

SiteFormat Format(const Candidate& candidate)
{
    return SiteFormat{candidate.id, candidate.bytes, candidate.referer, candidate.ext};
}

// The RFC 6381 list as far as the site named it.
std::string Codecs(const Candidate& candidate)
{
    std::string codecs;
    for (const auto& [codec, text] : {std::pair{candidate.video, &candidate.vcodec}, std::pair{candidate.audio, &candidate.acodec}}) {
        if (codec == Codec::None || codec == Codec::Unstated) {
            continue;
        }
        codecs += (codecs.empty() ? "" : ",") + *text;
    }
    return codecs;
}

Variant MakeVariant(const Picture& picture, std::optional<double> seconds)
{
    const Candidate& candidate = *picture.candidate;
    Variant variant;
    variant.bandwidth = Bandwidth(candidate, seconds);
    if (candidate.height > 0) {
        variant.resolution = Resolution{candidate.width, candidate.height};
    }
    if (candidate.fps > 0) {
        variant.frameRate = candidate.fps;
    }
    variant.codecs = Codecs(candidate);
    variant.mimeType = "video/" + Container(candidate);
    if (picture.separate) {
        variant.audioGroup = DeliveryGroup(candidate.delivery);
    }
    variant.source = Source(candidate, seconds);
    variant.site = Format(candidate);
    return variant;
}

Rendition MakeAudio(const Candidate& candidate, std::optional<double> seconds)
{
    Rendition rendition;
    rendition.name = candidate.note;
    rendition.language = candidate.language;
    rendition.bandwidth = Bandwidth(candidate, seconds);
    rendition.codecs = Codecs(candidate);
    rendition.mimeType = "audio/" + Container(candidate);
    rendition.source = Source(candidate, seconds);
    rendition.site = Format(candidate);
    return rendition;
}

// One language's subtitles: its WebVTT file, or SRT, which the join turns into WebVTT. Given as
// a file, or as an HLS playlist of pieces (TED).
std::optional<Rendition> ReadSubtitleTrack(const Json& entries, const std::string& language, const std::string& id,
                                           const std::string& suffix)
{
    if (!entries.is_array()) {
        return std::nullopt;
    }
    const Json* chosen = nullptr;
    for (const char* wanted : {"vtt", "srt"}) {
        for (const Json& entry : entries) {
            if (entry.is_object() && Text(entry, "ext") == wanted) {
                chosen = &entry;
                break;
            }
        }
        if (chosen != nullptr) {
            break;
        }
    }
    if (chosen == nullptr) {
        return std::nullopt;
    }
    const std::string url = Text(*chosen, "url");
    const auto checked = CheckedUrl(url, url);
    if (!checked) {
        return std::nullopt;
    }
    const std::string protocol = Text(*chosen, "protocol");
    Rendition rendition;
    if (protocol.empty() || protocol == "https" || protocol == "http") {
        TrackSegments file;
        Segment segment;
        segment.url = *checked;
        file.segments.push_back(std::move(segment));
        rendition.source = std::move(file);
    } else if (protocol == "m3u8_native" || protocol == "m3u8") {
        rendition.source = PlaylistAddress{*checked};
    } else {
        return std::nullopt;
    }
    std::string name = Text(*chosen, "name");
    if (name.empty() || !PlainText(name, 128)) {
        name = language;
    }
    rendition.name = name + suffix;
    rendition.language = language;
    rendition.codecs = "wvtt";
    rendition.site = SiteFormat{id, std::nullopt, std::string(), Text(*chosen, "ext")};
    return rendition;
}

// The uploader's subtitles in every language, then the captions the site made automatically in
// the video's own language ("-orig" on YouTube); not the machine translations into every other
// language (about 150 per YouTube video).
void ReadSubtitles(const Json& root, MediaOffer& offer)
{
    constexpr size_t kMaxSubtitleLanguages = 200;
    const auto read = [&offer](const Json& languages, bool automatic) {
        if (!languages.is_object()) {
            return;
        }
        for (const auto& [key, entries] : languages.items()) {
            if (offer.subtitles.size() >= kMaxSubtitleLanguages) {
                return;
            }
            if (key.empty() || !PlainText(key, 40) || key == "live_chat") {
                continue;
            }
            std::string language = key;
            if (automatic) {
                if (!key.ends_with("-orig")) {
                    continue;
                }
                language.resize(key.size() - 5);
            }
            const std::string id = (automatic ? "automatic:" : "subtitles:") + key;
            if (auto rendition = ReadSubtitleTrack(entries, language, id, automatic ? " (automatic)" : "")) {
                offer.subtitles.push_back(std::move(*rendition));
            }
        }
    };
    if (const auto subtitles = root.find("subtitles"); subtitles != root.end()) {
        read(*subtitles, false);
    }
    if (const auto automatic = root.find("automatic_captions"); automatic != root.end()) {
        read(*automatic, true);
    }
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
    // A live stream is recorded from its HLS playlists, as a live HLS stream of the media panel
    // (checklist P-07); its other formats are left out.
    const bool live = Flag(root, "is_live") || liveStatus == "is_live";

    SiteVideo video;
    video.id = Text(root, "id");
    if (video.id.empty() || video.id.size() > 256) {
        return Unreadable();
    }
    video.title = Text(root, "title");
    if (video.title.size() > 4096) {
        video.title.clear();
    }
    video.extractor = Text(root, "extractor_key");
    if (!PlainText(video.extractor, 64)) {
        video.extractor.clear();
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
        if (!candidate || (live && candidate->delivery != Delivery::Hls)) {
            continue;
        }
        ReadCookies((*formats)[i], video.cookies);
        if (candidate->audio == Codec::None) {
            videoOnly.push_back(std::move(*candidate));
        } else if (candidate->video == Codec::None) {
            audio.push_back(std::move(*candidate));
        } else {
            combined.push_back(std::move(*candidate));  // sound inside, or not said: kept with the picture
        }
    }

    // The best stereo track per delivery, language and codec; surround and compressed copies are
    // left out.
    std::map<std::tuple<Delivery, std::string, Codec>, size_t> bestAudio;
    for (size_t i = 0; i < audio.size(); ++i) {
        if (IsCompressedAudio(audio[i]) || audio[i].channels > 2) {
            continue;
        }
        const auto key = std::make_tuple(audio[i].delivery, audio[i].language, audio[i].audio);
        const auto found = bestAudio.find(key);
        if (found == bestAudio.end() || audio[i].kbps > audio[found->second].kbps) {
            bestAudio[key] = i;
        }
    }
    std::set<Delivery> withAudio;
    for (const auto& [key, index] : bestAudio) {
        withAudio.insert(std::get<0>(key));
    }

    // One picture per height and frame rate class: from pictures whose sound can be paired (a
    // track of the same delivery) and from pictures with their own sound.
    std::vector<Picture> pictures;
    for (const Candidate& candidate : videoOnly) {
        if (withAudio.contains(candidate.delivery)) {
            pictures.push_back(Picture{&candidate, true});
        }
    }
    for (const Candidate& candidate : combined) {
        pictures.push_back(Picture{&candidate, false});
    }
    std::map<std::pair<int, int>, size_t> bestByPicture;
    for (size_t i = 0; i < pictures.size(); ++i) {
        const auto key = std::make_pair(pictures[i].candidate->height, FrameRateClass(pictures[i].candidate->fps));
        const auto found = bestByPicture.find(key);
        if (found == bestByPicture.end()) {
            bestByPicture.emplace(key, i);
        } else if (BetterPicture(pictures[i], pictures[found->second])) {
            found->second = i;
        }
    }
    // A picture of unknown size is offered only when no picture has a size: it cannot be placed
    // among them sensibly.
    if (std::any_of(bestByPicture.begin(), bestByPicture.end(), [](const auto& entry) { return entry.first.first > 0; })) {
        std::erase_if(bestByPicture, [](const auto& entry) { return entry.first.first == 0; });
    }

    MediaOffer& offer = video.offer;
    // The pairing goes by groups (HLS's rule in the quality list), so pictures with their own sound
    // and pictures with separate sound can be offered side by side.
    offer.format = StreamFormat::Hls;
    offer.live = live;
    offer.seconds = live ? std::nullopt : video.seconds;
    std::set<Delivery> paired;
    for (const auto& [key, index] : bestByPicture) {
        offer.variants.push_back(MakeVariant(pictures[index], video.seconds));
        if (pictures[index].separate) {
            paired.insert(pictures[index].candidate->delivery);
        }
    }
    // Highest picture first, as the quality list sorts them too; keeps the offer readable in logs.
    std::reverse(offer.variants.begin(), offer.variants.end());

    // Per delivery, the track paired with every picture: the site's preferred language (the
    // original audio of a dubbed video), in AAC, which MP4 players all take.
    std::map<Delivery, size_t> pairedIndex;
    for (const auto& [key, index] : bestAudio) {
        const Delivery delivery = std::get<0>(key);
        const auto current = pairedIndex.find(delivery);
        if (current == pairedIndex.end()) {
            pairedIndex.emplace(delivery, index);
            continue;
        }
        const auto rank = [](const Candidate& c) { return std::make_tuple(c.languagePreference, c.audio == Codec::Aac, c.kbps); };
        if (rank(audio[index]) > rank(audio[current->second])) {
            current->second = index;
        }
    }
    for (const auto& [key, index] : bestAudio) {
        const Delivery delivery = std::get<0>(key);
        // Sound no picture pairs with is offered on its own, unless the same language comes in a
        // better delivery too (YouTube's HLS copies of its files).
        const bool better = std::any_of(bestAudio.begin(), bestAudio.end(), [&key](const auto& other) {
            return std::get<1>(other.first) == std::get<1>(key) &&
                   DeliveryRank(std::get<0>(other.first)) < DeliveryRank(std::get<0>(key));
        });
        if (!paired.contains(delivery) && better) {
            continue;
        }
        Rendition rendition = MakeAudio(audio[index], video.seconds);
        if (paired.contains(delivery)) {
            rendition.groupId = DeliveryGroup(delivery);
            rendition.isDefault = pairedIndex.at(delivery) == index;
        }
        offer.audio.push_back(std::move(rendition));
    }

    if (offer.variants.empty() && offer.audio.empty()) {
        if (live) {
            return Refused("This live stream offers no way of recording it that LDM has.");
        }
        return Refused(protectedSeen ? "This video is protected and cannot be downloaded."
                                     : "This video has no format LDM can download.");
    }
    ReadSubtitles(root, video.offer);
    return video;
}

}  // namespace ldm::media
