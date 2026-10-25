#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// What LDM knows about a streamed video once its manifest is read (M3 design section 6), the same
// for HLS and DASH. Two levels, because HLS needs a second fetch between them: a MediaOffer lists
// what can be downloaded (the quality list), and a TrackSegments lists what one track is made of
// (the download list). Everything here comes from a manifest a web site served, so the parsers
// fill it only with checked values: absolute http(s) addresses, sane numbers, bounded counts.
namespace ldm::media {

// Upper bounds on what one manifest may make LDM do, against hostile or broken manifests.
inline constexpr size_t kMaxManifestBytes = 16 * 1024 * 1024;
// 200,000 two-second segments are 111 hours; a manifest claiming more is refused (or, for a
// live DASH stream with no window, cut to its most recent segments).
inline constexpr size_t kMaxSegmentsPerTrack = 200'000;

struct ByteRange {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;  // at least 1
};

struct InitSection {
    std::string url;
    std::optional<ByteRange> range;
};

// A key for AES-128 (whole segments, CBC with PKCS#7 padding, HLS METHOD=AES-128). Either an
// address LDM fetches the 16 bytes from, or the bytes themselves when the playlist embedded them
// in a data: address.
struct EncryptionKey {
    std::string url;  // empty when `value` is set
    std::optional<std::array<std::uint8_t, 16>> value;
};

struct Segment {
    std::string url;
    std::optional<ByteRange> range;  // a part of `url`; the whole file otherwise
    double seconds = 0;
    // HLS: the media sequence number. DASH: the segment's number ($Number$, from startNumber).
    std::uint64_t sequence = 0;
    // Starts a new timeline: timestamps, codec settings or the init section may change here (HLS
    // EXT-X-DISCONTINUITY, or the first segment of a later DASH period).
    bool discontinuity = false;
    std::optional<size_t> init;  // index into TrackSegments::inits; none for self-contained segments
    std::optional<size_t> key;   // index into TrackSegments::keys; none for clear segments
    std::array<std::uint8_t, 16> iv{};  // the AES-128 IV, worked out by the parser; zero when clear
};

// DRM detected (checklist B-09). LDM shows the notice and downloads nothing.
struct DrmInfo {
    // What was found, for the notice: "Widevine", "PlayReady", "FairPlay", "SAMPLE-AES", or the
    // manifest's own identifier when LDM does not know the system.
    std::vector<std::string> systems;
};

// One track's download list.
struct TrackSegments {
    std::vector<InitSection> inits;
    std::vector<EncryptionKey> keys;
    std::vector<Segment> segments;
    double targetSeconds = 0;  // HLS EXT-X-TARGETDURATION; DASH: the longest segment
    // More segments will be published: an HLS playlist without EXT-X-ENDLIST, a dynamic MPD.
    bool live = false;
    // Set for protected content. Such a track carries no inits, keys or segments, so nothing later
    // can download it by mistake.
    std::optional<DrmInfo> drm;
};

[[nodiscard]] double TotalSeconds(const TrackSegments& track);

// Where a track's segment list comes from: an HLS media playlist still to be fetched (and read
// with ParseHls), or a DASH representation the MPD already described in full.
struct PlaylistAddress {
    std::string url;
};
using SegmentSource = std::variant<PlaylistAddress, TrackSegments>;

struct Resolution {
    int width = 0;
    int height = 0;
};

// A format of a video site's page, as the site resolver named it (SiteVideo.h): what to ask for
// again when the format's address has expired, and how to tell that the same bytes came back.
struct SiteFormat {
    std::string id;                     // the resolver's format id, such as "299"
    std::optional<std::int64_t> bytes;  // the exact length, when the site states one
    std::string referer;                // the Referer the site wants for this format; empty for none
    std::string ext;                    // the file type the site names, such as "mp4" or "ogv"
    // Other headers the site wants with this format's requests, as "Name: value" lines (a
    // session id, a stream key); checked when read (SiteVideo.h).
    std::vector<std::string> headers;
};

// One quality of the video, or of the whole stream when it has no video (an audio-only HLS
// variant, say). An HLS variant may carry its own audio; its codecs say so.
struct Variant {
    std::uint64_t bandwidth = 0;  // bits per second: HLS peak BANDWIDTH, DASH @bandwidth
    std::optional<std::uint64_t> averageBandwidth;
    std::optional<Resolution> resolution;
    std::optional<double> frameRate;
    std::string codecs;    // RFC 6381 list as the manifest gave it; may be empty for HLS
    std::string mimeType;  // DASH only, such as "video/mp4" or "video/webm"
    // HLS GROUP-IDs of the renditions this variant plays with; empty for none. DASH has no groups:
    // any of its audio and subtitles go with any video, so these stay empty.
    std::string audioGroup;
    std::string subtitleGroup;
    SegmentSource source;
    std::optional<SiteFormat> site;  // set only in a site's offer
};

// An alternative audio track or a subtitle track.
struct Rendition {
    std::string groupId;  // HLS GROUP-ID; DASH: the adaptation set's id (or its position)
    std::string name;
    std::string language;  // as the manifest gave it (BCP 47 in practice)
    bool isDefault = false;
    bool forced = false;  // subtitles shown only for foreign-language parts
    std::optional<int> channels;
    std::uint64_t bandwidth = 0;  // DASH only
    std::string codecs;
    std::string mimeType;  // DASH only
    // None for an HLS audio rendition without URI: that audio is inside the variant's own segments.
    std::optional<SegmentSource> source;
    std::optional<SiteFormat> site;  // set only in a site's offer
};

enum class StreamFormat {
    Hls,
    Dash,
};

// What a manifest offers.
struct MediaOffer {
    StreamFormat format = StreamFormat::Hls;
    std::vector<Variant> variants;
    std::vector<Rendition> audio;
    std::vector<Rendition> subtitles;
    // DASH: a dynamic MPD. An HLS master playlist cannot say; its media playlists do.
    bool live = false;
    std::optional<double> seconds;  // DASH mediaPresentationDuration, or its periods' sum
    std::optional<DrmInfo> drm;
};

}  // namespace ldm::media
