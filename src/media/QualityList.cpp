#include "media/QualityList.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>
#include <utility>
#include <variant>

namespace ldm::media {

namespace {

struct CodecName {
    std::string_view prefix;
    std::string_view family;
};

constexpr std::array<CodecName, 8> kVideoCodecs{{
    {"avc1", "H.264"}, {"avc3", "H.264"}, {"hvc1", "H.265"}, {"hev1", "H.265"},
    {"vp09", "VP9"},   {"vp9", "VP9"},    {"av01", "AV1"},   {"mp4v", "MPEG-4"},
}};
constexpr std::array<CodecName, 8> kAudioCodecs{{
    {"mp4a.40.34", "MP3"}, {"mp4a", "AAC"}, {"ac-3", "AC-3"}, {"ec-3", "E-AC-3"},
    {"opus", "Opus"},      {"vorbis", "Vorbis"}, {"flac", "FLAC"}, {"mp3", "MP3"},
}};

std::string Lower(std::string_view text)
{
    std::string lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; });
    return lower;
}

// The family of the first codec of the list that is in `names`.
template <size_t N>
std::string Family(const std::string& codecs, const std::array<CodecName, N>& names)
{
    size_t start = 0;
    while (start < codecs.size()) {
        const size_t comma = std::min(codecs.find(',', start), codecs.size());
        const std::string codec = Lower(codecs.substr(start, comma - start));
        const auto trimmed = codec.substr(std::min(codec.find_first_not_of(' '), codec.size()));
        for (const CodecName& name : names) {
            if (trimmed.starts_with(name.prefix)) {
                return std::string(name.family);
            }
        }
        start = comma + 1;
    }
    return {};
}

std::uint64_t Rate(const Variant& variant)
{
    return variant.averageBandwidth.value_or(variant.bandwidth);
}

// A variant that says nothing about its streams (a media playlist on its own, or a master playlist
// without CODECS and RESOLUTION) is taken to have a picture: sound saved as a video file still
// plays, a video saved as sound does not.
bool HasPicture(const Variant& variant)
{
    if (variant.resolution || variant.mimeType.starts_with("video/")) {
        return true;
    }
    if (!variant.codecs.empty()) {
        return !VideoCodecFamily(variant.codecs).empty();
    }
    return !variant.mimeType.starts_with("audio/");
}

bool CanDownload(const std::optional<SegmentSource>& source)
{
    return source.has_value();
}

// The audio track to pair with a variant whose sound is separate: within its HLS group (DASH has
// none, so any), the default one, otherwise the one with the highest bandwidth.
std::optional<size_t> PairedAudio(const MediaOffer& offer, const Variant& variant)
{
    std::optional<size_t> best;
    for (size_t i = 0; i < offer.audio.size(); ++i) {
        const Rendition& rendition = offer.audio[i];
        if (!CanDownload(rendition.source) || (!variant.audioGroup.empty() && rendition.groupId != variant.audioGroup)) {
            continue;
        }
        if (!best) {
            best = i;
            continue;
        }
        const Rendition& current = offer.audio[*best];
        if ((rendition.isDefault && !current.isDefault) ||
            (rendition.isDefault == current.isDefault && rendition.bandwidth > current.bandwidth)) {
            best = i;
        }
    }
    return best;
}

// The variant's sound is a separate track: DASH always, HLS when its audio group holds tracks
// with their own playlists.
bool SoundIsSeparate(const MediaOffer& offer, const Variant& variant)
{
    if (offer.format == StreamFormat::Dash) {
        return true;
    }
    return !variant.audioGroup.empty() && std::any_of(offer.audio.begin(), offer.audio.end(), [&variant](const Rendition& r) {
               return r.groupId == variant.audioGroup && r.source.has_value();
           });
}

std::optional<std::int64_t> Bytes(std::uint64_t bitsPerSecond, std::optional<double> seconds)
{
    if (!seconds || *seconds <= 0 || bitsPerSecond == 0) {
        return std::nullopt;
    }
    const double bytes = static_cast<double>(bitsPerSecond) * *seconds / 8;
    if (!std::isfinite(bytes) || bytes > 9e18) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(bytes);
}

}  // namespace

std::string VideoCodecFamily(const std::string& codecs)
{
    return Family(codecs, kVideoCodecs);
}

std::string AudioCodecFamily(const std::string& codecs)
{
    return Family(codecs, kAudioCodecs);
}

QualityList BuildQualityList(const MediaOffer& offer, std::optional<double> seconds)
{
    QualityList list;
    if (offer.drm) {
        list.drm = offer.drm;
        return list;
    }
    std::vector<QualityEntry> video;
    std::vector<QualityEntry> videoOnly;
    std::vector<QualityEntry> audioOnly;
    for (size_t i = 0; i < offer.variants.size(); ++i) {
        const Variant& variant = offer.variants[i];
        QualityEntry entry;
        entry.variant = i;
        entry.videoCodec = VideoCodecFamily(variant.codecs);
        entry.audioCodec = AudioCodecFamily(variant.codecs);
        entry.bitsPerSecond = Rate(variant);
        entry.statedVideo = variant.mimeType.starts_with("video/");
        if (!HasPicture(variant)) {
            entry.kind = QualityKind::AudioOnly;  // an HLS variant with sound only
            audioOnly.push_back(entry);
            continue;
        }
        entry.height = variant.resolution ? std::optional<int>(variant.resolution->height) : std::nullopt;
        entry.frameRate = variant.frameRate;
        if (SoundIsSeparate(offer, variant)) {
            QualityEntry pictureOnly = entry;
            pictureOnly.kind = QualityKind::VideoOnly;
            pictureOnly.audioCodec.clear();
            videoOnly.push_back(pictureOnly);
            if (const auto audio = PairedAudio(offer, variant)) {
                const Rendition& rendition = offer.audio[*audio];
                entry.audio = audio;
                // HLS tracks rarely name their codec; the variant's CODECS covers its sound then.
                if (std::string codec = AudioCodecFamily(rendition.codecs); !codec.empty()) {
                    entry.audioCodec = std::move(codec);
                }
                entry.language = rendition.language;
                entry.bitsPerSecond += rendition.bandwidth;
            }
        }
        entry.kind = QualityKind::Video;
        video.push_back(entry);
    }
    for (size_t i = 0; i < offer.audio.size(); ++i) {
        const Rendition& rendition = offer.audio[i];
        if (!CanDownload(rendition.source)) {
            continue;  // sound inside the variants
        }
        QualityEntry entry;
        entry.kind = QualityKind::AudioOnly;
        entry.audio = i;
        entry.audioCodec = AudioCodecFamily(rendition.codecs);
        entry.language = rendition.language;
        entry.bitsPerSecond = rendition.bandwidth;
        audioOnly.push_back(entry);
    }
    const auto byPicture = [](const QualityEntry& a, const QualityEntry& b) {
        if (a.height.value_or(0) != b.height.value_or(0)) {
            return a.height.value_or(0) > b.height.value_or(0);
        }
        return a.bitsPerSecond > b.bitsPerSecond;
    };
    std::stable_sort(video.begin(), video.end(), byPicture);
    std::stable_sort(videoOnly.begin(), videoOnly.end(), byPicture);
    std::stable_sort(audioOnly.begin(), audioOnly.end(),
                     [](const QualityEntry& a, const QualityEntry& b) { return a.bitsPerSecond > b.bitsPerSecond; });
    for (auto* group : {&video, &videoOnly, &audioOnly}) {
        for (QualityEntry& entry : *group) {
            entry.approximateBytes = Bytes(entry.bitsPerSecond, seconds);
            list.entries.push_back(std::move(entry));
        }
    }
    for (size_t i = 0; i < offer.subtitles.size(); ++i) {
        if (offer.subtitles[i].source) {
            list.subtitles.push_back(i);
        }
    }
    return list;
}

std::string QualityLabel(const QualityEntry& entry)
{
    std::string label;
    const auto add = [&label](std::string_view part) {
        if (part.empty()) {
            return;
        }
        if (!label.empty()) {
            label += ' ';
        }
        label += part;
    };
    if (entry.kind == QualityKind::VideoOnly) {
        add("Video only");
    } else if (entry.kind == QualityKind::AudioOnly) {
        add("Audio");
        add(entry.language);
    }
    if (entry.kind != QualityKind::AudioOnly) {
        if (entry.height) {
            std::string picture = std::to_string(*entry.height) + "p";
            if (entry.frameRate && *entry.frameRate > 31) {
                picture += std::to_string(static_cast<int>(std::lround(*entry.frameRate)));
            }
            add(picture);
        }
        add(entry.videoCodec);
    }
    if (entry.kind == QualityKind::AudioOnly) {
        add(entry.audioCodec);
        if (entry.bitsPerSecond > 0) {
            add(std::to_string((entry.bitsPerSecond + 500) / 1000) + " kbps");
        }
    } else if (!entry.height && entry.bitsPerSecond > 0) {
        add(std::to_string((entry.bitsPerSecond + 500) / 1000) + " kbps");  // no size to tell variants apart
    }
    if (label.empty()) {
        // Nothing about the picture is known: a site's file (a clip that states no size) is still
        // known to be a video, a bare HLS playlist not even that.
        label = entry.statedVideo ? "Video" : "Media";
    }
    return label;
}

}  // namespace ldm::media
