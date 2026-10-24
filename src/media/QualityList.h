#pragma once

#include "media/MediaModel.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// What the media panel offers for one stream (checklist B-07): the qualities to choose from,
// best first, and the subtitles that can come with them.
namespace ldm::media {

enum class QualityKind {
    Video,      // the picture with its sound: a variant, plus an audio track when it has none of its own
    VideoOnly,  // the picture of a variant whose sound is a separate track, without it
    AudioOnly,  // a separate audio track, or an HLS variant that has no picture
};

struct QualityEntry {
    QualityKind kind = QualityKind::Video;
    std::optional<size_t> variant;  // index into MediaOffer::variants
    std::optional<size_t> audio;    // index into MediaOffer::audio
    std::optional<int> height;      // of the picture
    std::optional<double> frameRate;
    std::string videoCodec;  // a family name such as "H.264"; empty when unknown or none
    std::string audioCodec;  // such as "AAC"
    std::string language;    // of the audio track, when it is a separate one
    std::uint64_t bitsPerSecond = 0;  // picture and sound together, as the manifest states them
    std::optional<std::int64_t> approximateBytes;  // from bitsPerSecond and the length, when known
};

struct QualityList {
    std::vector<QualityEntry> entries;
    std::vector<size_t> subtitles;  // indexes into MediaOffer::subtitles that have a source
    std::optional<DrmInfo> drm;     // set: nothing can be downloaded; entries is empty
};

// `seconds` is the stream's length when known (an MPD states it; for HLS it comes from a media
// playlist). Video entries come first, highest picture first; each variant gets the default
// audio track (or the best one) when its sound is separate.
[[nodiscard]] QualityList BuildQualityList(const MediaOffer& offer, std::optional<double> seconds);

// The family of the first video or audio codec in an RFC 6381 list ("avc1.640028,mp4a.40.2"
// gives "H.264" and "AAC"); empty when the list names none LDM knows.
[[nodiscard]] std::string VideoCodecFamily(const std::string& codecs);
[[nodiscard]] std::string AudioCodecFamily(const std::string& codecs);

// The words the panel shows for an entry, without the size: "1080p60 H.264", "Video only 720p
// VP9", "Audio English AAC 128 kbps".
[[nodiscard]] std::string QualityLabel(const QualityEntry& entry);

}  // namespace ldm::media
