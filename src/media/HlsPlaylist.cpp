#include "media/HlsPlaylist.h"

#include "base/Base64.h"
#include "media/DrmSystems.h"
#include "media/NumberText.h"
#include "media/UrlReference.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace ldm::media {

namespace {

constexpr std::string_view kBom = "\xEF\xBB\xBF";
// Bounds for numbers LDM keeps in an int.
constexpr std::uint64_t kMaxPictureSide = 100'000;
constexpr std::uint64_t kMaxChannels = 64;

struct Line {
    std::string_view text;
    size_t number = 0;  // 1-based, for messages
};

base::Error LineError(size_t line, std::string_view message)
{
    return base::MakeError(base::ErrorCode::Protocol, "line " + std::to_string(line) + ": " + std::string(message));
}

std::string_view Trim(std::string_view text)
{
    const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    while (!text.empty() && isSpace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && isSpace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

// Non-empty lines, trimmed. Lines end with LF or CRLF.
std::vector<Line> SplitLines(std::string_view text)
{
    std::vector<Line> lines;
    size_t number = 0;
    while (!text.empty()) {
        ++number;
        const size_t end = std::min(text.find('\n'), text.size());
        const std::string_view line = Trim(text.substr(0, end));
        if (!line.empty()) {
            lines.push_back(Line{line, number});
        }
        text.remove_prefix(std::min(end + 1, text.size()));
    }
    return lines;
}

bool IsTag(std::string_view line)
{
    return line.starts_with("#EXT");
}

// "#EXT-X-KEY:METHOD=..." -> "EXT-X-KEY" and "METHOD=...".
std::pair<std::string_view, std::string_view> SplitTag(std::string_view line)
{
    line.remove_prefix(1);
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
        return {line, {}};
    }
    return {line.substr(0, colon), line.substr(colon + 1)};
}

// An attribute list (RFC 8216 section 4.2): NAME=VALUE pairs separated by commas, where a quoted
// value may contain commas. Values are kept as written, quotes removed.
class AttributeList {
public:
    static base::Result<AttributeList> Parse(std::string_view text)
    {
        AttributeList list;
        size_t pos = 0;
        const auto skipSpaces = [&text, &pos]() {
            while (pos < text.size() && text[pos] == ' ') {
                ++pos;
            }
        };
        while (true) {
            skipSpaces();
            if (pos >= text.size()) {
                return list;
            }
            const size_t nameStart = pos;
            while (pos < text.size() && ((text[pos] >= 'A' && text[pos] <= 'Z') ||
                                         (text[pos] >= '0' && text[pos] <= '9') || text[pos] == '-')) {
                ++pos;
            }
            const std::string_view name = text.substr(nameStart, pos - nameStart);
            if (name.empty() || pos >= text.size() || text[pos] != '=') {
                return base::MakeError(base::ErrorCode::Protocol, "malformed attribute list");
            }
            ++pos;
            std::string_view value;
            if (pos < text.size() && text[pos] == '"') {
                const size_t close = text.find('"', pos + 1);
                if (close == std::string_view::npos) {
                    return base::MakeError(base::ErrorCode::Protocol, "unterminated quoted value of " + std::string(name));
                }
                value = text.substr(pos + 1, close - pos - 1);
                pos = close + 1;
            } else {
                const size_t end = std::min(text.find(',', pos), text.size());
                value = Trim(text.substr(pos, end - pos));
                pos = end;
            }
            if (list.Find(name)) {
                return base::MakeError(base::ErrorCode::Protocol, "attribute " + std::string(name) + " appears twice");
            }
            list.values_.emplace_back(std::string(name), std::string(value));
            skipSpaces();
            if (pos < text.size()) {
                if (text[pos] != ',') {
                    return base::MakeError(base::ErrorCode::Protocol, "malformed attribute list");
                }
                ++pos;
            }
        }
    }

    [[nodiscard]] const std::string* Find(std::string_view name) const
    {
        const auto found = std::find_if(values_.begin(), values_.end(),
                                        [name](const auto& entry) { return entry.first == name; });
        return found == values_.end() ? nullptr : &found->second;
    }

    [[nodiscard]] std::string Text(std::string_view name) const
    {
        const std::string* value = Find(name);
        return value ? *value : std::string();
    }

    [[nodiscard]] bool IsYes(std::string_view name) const
    {
        const std::string* value = Find(name);
        return value && *value == "YES";
    }

private:
    std::vector<std::pair<std::string, std::string>> values_;
};

std::optional<Resolution> ParseResolution(std::string_view text)
{
    const size_t x = text.find_first_of("xX");
    if (x == std::string_view::npos) {
        return std::nullopt;
    }
    const auto width = ParseUnsigned(text.substr(0, x));
    const auto height = ParseUnsigned(text.substr(x + 1));
    if (!width || !height || *width == 0 || *height == 0 || *width > kMaxPictureSide || *height > kMaxPictureSide) {
        return std::nullopt;
    }
    return Resolution{static_cast<int>(*width), static_cast<int>(*height)};
}

// "0x" followed by exactly 32 hex digits: the 128-bit IV.
std::optional<std::array<std::uint8_t, 16>> ParseIv(std::string_view text)
{
    if (text.size() != 34 || text[0] != '0' || (text[1] != 'x' && text[1] != 'X')) {
        return std::nullopt;
    }
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    std::array<std::uint8_t, 16> iv{};
    for (size_t i = 0; i < iv.size(); ++i) {
        const int high = nibble(text[2 + 2 * i]);
        const int low = nibble(text[3 + 2 * i]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        iv[i] = static_cast<std::uint8_t>(high * 16 + low);
    }
    return iv;
}

// RFC 8216 section 5.2: without an IV attribute, the IV is the segment's media sequence number as
// a 128-bit big-endian integer.
std::array<std::uint8_t, 16> SequenceIv(std::uint64_t sequence)
{
    std::array<std::uint8_t, 16> iv{};
    for (size_t i = 0; i < 8; ++i) {
        iv[15 - i] = static_cast<std::uint8_t>((sequence >> (8 * i)) & 0xFF);
    }
    return iv;
}

// "<length>[@<offset>]".
struct RangeText {
    std::uint64_t length = 0;
    std::optional<std::uint64_t> offset;
};

std::optional<RangeText> ParseRangeText(std::string_view text)
{
    const size_t at = text.find('@');
    RangeText range;
    const auto length = ParseUnsigned(text.substr(0, at));
    if (!length || *length == 0) {
        return std::nullopt;
    }
    range.length = *length;
    if (at != std::string_view::npos) {
        range.offset = ParseUnsigned(text.substr(at + 1));
        if (!range.offset) {
            return std::nullopt;
        }
    }
    return range;
}

// "data:[<media type>];base64,<16 bytes in base64>", the only data: form a key may take.
std::optional<std::array<std::uint8_t, 16>> DecodeDataKey(std::string_view uri)
{
    const size_t comma = uri.find(',');
    if (comma == std::string_view::npos || !uri.substr(0, comma).ends_with(";base64")) {
        return std::nullopt;
    }
    const auto bytes = base::DecodeBase64(uri.substr(comma + 1));
    if (!bytes || bytes->size() != 16) {
        return std::nullopt;
    }
    std::array<std::uint8_t, 16> key{};
    std::transform(bytes->begin(), bytes->end(), key.begin(), [](char c) { return static_cast<std::uint8_t>(c); });
    return key;
}

bool IsDataUri(std::string_view uri)
{
    if (uri.size() < 5) {
        return false;
    }
    std::string scheme(uri.substr(0, 5));
    std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                   [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; });
    return scheme == "data:";
}

// The DRM system behind an EXT-X-KEY or EXT-X-SESSION-KEY, or nothing when LDM can decrypt it
// itself (no encryption, or AES-128 with a plain key).
std::optional<std::string> ProtectionOf(const AttributeList& key)
{
    const std::string method = key.Text("METHOD");
    const std::string* format = key.Find("KEYFORMAT");
    if (method == "NONE") {
        return std::nullopt;
    }
    if (format && *format != "identity") {
        return DrmSystemName(*format);
    }
    if (method == "AES-128") {
        return std::nullopt;
    }
    if (method == "SAMPLE-AES" || method == "SAMPLE-AES-CTR") {
        return std::string("SAMPLE-AES");
    }
    // An encryption method LDM does not know cannot be decrypted either.
    return method.empty() ? std::string("unknown encryption") : method;
}

enum class PlaylistKind {
    Master,
    Media,
};

base::Result<PlaylistKind> KindOf(const std::vector<Line>& lines)
{
    std::optional<size_t> masterLine;
    std::optional<size_t> mediaLine;
    for (const Line& line : lines) {
        if (!IsTag(line.text)) {
            continue;
        }
        const auto tag = SplitTag(line.text).first;
        if (tag == "EXT-X-STREAM-INF" || tag == "EXT-X-MEDIA" || tag == "EXT-X-I-FRAME-STREAM-INF" ||
            tag == "EXT-X-SESSION-KEY" || tag == "EXT-X-SESSION-DATA") {
            masterLine = masterLine.value_or(line.number);
        } else if (tag == "EXTINF" || tag == "EXT-X-TARGETDURATION" || tag == "EXT-X-MEDIA-SEQUENCE" ||
                   tag == "EXT-X-ENDLIST" || tag == "EXT-X-KEY" || tag == "EXT-X-MAP" || tag == "EXT-X-BYTERANGE" ||
                   tag == "EXT-X-DISCONTINUITY" || tag == "EXT-X-PLAYLIST-TYPE") {
            mediaLine = mediaLine.value_or(line.number);
        }
    }
    if (masterLine && mediaLine) {
        return LineError(std::max(*masterLine, *mediaLine), "the playlist mixes master and media playlist tags");
    }
    if (masterLine) {
        return PlaylistKind::Master;
    }
    if (mediaLine) {
        return PlaylistKind::Media;
    }
    return base::MakeError(base::ErrorCode::Protocol, "the playlist lists neither variants nor segments");
}

base::Result<AttributeList> TagAttributes(const Line& line, std::string_view value)
{
    auto attributes = AttributeList::Parse(value);
    if (!attributes) {
        return LineError(line.number, attributes.GetError().message);
    }
    return attributes;
}

base::Result<MediaOffer> ParseMaster(const std::vector<Line>& lines, const std::string& baseUrl)
{
    MediaOffer offer;
    offer.format = StreamFormat::Hls;
    std::optional<std::pair<AttributeList, size_t>> pendingVariant;

    for (const Line& line : lines) {
        if (!IsTag(line.text)) {
            if (line.text.starts_with('#')) {
                continue;  // a comment
            }
            if (!pendingVariant) {
                return LineError(line.number, "an address that follows no EXT-X-STREAM-INF");
            }
            const AttributeList& attributes = pendingVariant->first;
            Variant variant;
            const auto bandwidth = ParseUnsigned(attributes.Text("BANDWIDTH"));
            if (!bandwidth) {
                return LineError(pendingVariant->second, "EXT-X-STREAM-INF needs a numeric BANDWIDTH");
            }
            variant.bandwidth = *bandwidth;
            if (const std::string* average = attributes.Find("AVERAGE-BANDWIDTH")) {
                variant.averageBandwidth = ParseUnsigned(*average);
                if (!variant.averageBandwidth) {
                    return LineError(pendingVariant->second, "AVERAGE-BANDWIDTH is not a number");
                }
            }
            if (const std::string* resolution = attributes.Find("RESOLUTION")) {
                variant.resolution = ParseResolution(*resolution);
                if (!variant.resolution) {
                    return LineError(pendingVariant->second, "RESOLUTION is not WIDTHxHEIGHT");
                }
            }
            if (const std::string* frameRate = attributes.Find("FRAME-RATE")) {
                variant.frameRate = ParseNonNegativeDecimal(*frameRate);
                if (!variant.frameRate || *variant.frameRate == 0) {
                    return LineError(pendingVariant->second, "FRAME-RATE is not a positive number");
                }
            }
            variant.codecs = attributes.Text("CODECS");
            variant.audioGroup = attributes.Text("AUDIO");
            variant.subtitleGroup = attributes.Text("SUBTITLES");
            auto url = ResolveMediaUrl(baseUrl, line.text);
            if (!url) {
                return LineError(line.number, "the variant's address is not an http or https address");
            }
            variant.source = PlaylistAddress{std::move(*url)};
            offer.variants.push_back(std::move(variant));
            pendingVariant.reset();
            continue;
        }

        const auto [tag, value] = SplitTag(line.text);
        if (pendingVariant && tag != "EXT-X-STREAM-INF") {
            // Tags between EXT-X-STREAM-INF and its address are not allowed, but harmless.
            continue;
        }
        if (tag == "EXT-X-STREAM-INF") {
            if (pendingVariant) {
                return LineError(line.number, "EXT-X-STREAM-INF follows another without an address between");
            }
            auto attributes = TagAttributes(line, value);
            if (!attributes) {
                return attributes.GetError();
            }
            pendingVariant.emplace(std::move(attributes).Value(), line.number);
        } else if (tag == "EXT-X-MEDIA") {
            auto parsed = TagAttributes(line, value);
            if (!parsed) {
                return parsed.GetError();
            }
            const AttributeList& attributes = parsed.Value();
            const std::string type = attributes.Text("TYPE");
            if (type != "AUDIO" && type != "SUBTITLES") {
                continue;  // VIDEO and CLOSED-CAPTIONS, see the header
            }
            Rendition rendition;
            rendition.groupId = attributes.Text("GROUP-ID");
            rendition.name = attributes.Text("NAME");
            if (rendition.groupId.empty() || rendition.name.empty()) {
                return LineError(line.number, "EXT-X-MEDIA needs GROUP-ID and NAME");
            }
            rendition.language = attributes.Text("LANGUAGE");
            rendition.isDefault = attributes.IsYes("DEFAULT");
            rendition.forced = attributes.IsYes("FORCED");
            if (const std::string* channels = attributes.Find("CHANNELS")) {
                // "2", or "16/JOC" for Dolby Atmos: the count comes first.
                const auto count = ParseUnsigned(std::string_view(*channels).substr(0, channels->find('/')));
                if (!count || *count == 0 || *count > kMaxChannels) {
                    return LineError(line.number, "CHANNELS does not start with a channel count");
                }
                rendition.channels = static_cast<int>(*count);
            }
            if (const std::string* uri = attributes.Find("URI")) {
                auto url = uri->empty() ? std::nullopt : ResolveMediaUrl(baseUrl, *uri);
                if (!url) {
                    return LineError(line.number, "the rendition's address is not an http or https address");
                }
                rendition.source = SegmentSource{PlaylistAddress{std::move(*url)}};
            } else if (type == "SUBTITLES") {
                return LineError(line.number, "a SUBTITLES rendition needs a URI");
            }
            (type == "AUDIO" ? offer.audio : offer.subtitles).push_back(std::move(rendition));
        } else if (tag == "EXT-X-SESSION-KEY") {
            auto attributes = TagAttributes(line, value);
            if (!attributes) {
                return attributes.GetError();
            }
            if (auto system = ProtectionOf(attributes.Value())) {
                AddDrmSystem(offer.drm, std::move(*system));
            }
        }
    }
    if (pendingVariant) {
        return LineError(pendingVariant->second, "EXT-X-STREAM-INF is not followed by an address");
    }

    // A variant naming a group the playlist does not define plays without one (the RFC forbids
    // this; players tolerate it, and so does LDM).
    const auto hasGroup = [](const std::vector<Rendition>& renditions, const std::string& group) {
        return std::any_of(renditions.begin(), renditions.end(),
                           [&group](const Rendition& rendition) { return rendition.groupId == group; });
    };
    for (Variant& variant : offer.variants) {
        if (!variant.audioGroup.empty() && !hasGroup(offer.audio, variant.audioGroup)) {
            variant.audioGroup.clear();
        }
        if (!variant.subtitleGroup.empty() && !hasGroup(offer.subtitles, variant.subtitleGroup)) {
            variant.subtitleGroup.clear();
        }
    }
    return offer;
}

// What the tags before a segment's address said about it.
struct PendingSegment {
    std::optional<double> seconds;
    size_t extinfLine = 0;
    std::optional<RangeText> range;
    bool discontinuity = false;
};

base::Result<TrackSegments> ParseMedia(const std::vector<Line>& lines, const std::string& baseUrl)
{
    TrackSegments track;
    std::optional<DrmInfo> drm;
    bool targetSeen = false;
    bool endList = false;
    std::uint64_t mediaSequence = 0;
    PendingSegment pending;
    std::optional<size_t> currentInit;
    std::optional<size_t> currentKey;
    std::optional<std::array<std::uint8_t, 16>> currentIv;
    // Where the previous segment's byte range ended, for a range without an offset.
    std::optional<std::pair<std::string, std::uint64_t>> previousRangeEnd;

    for (const Line& line : lines) {
        if (!IsTag(line.text)) {
            if (line.text.starts_with('#')) {
                continue;
            }
            if (!pending.seconds) {
                return LineError(line.number, "a segment address without EXTINF");
            }
            Segment segment;
            auto url = ResolveMediaUrl(baseUrl, line.text);
            if (!url) {
                return LineError(line.number, "the segment's address is not an http or https address");
            }
            segment.url = std::move(*url);
            segment.seconds = *pending.seconds;
            segment.discontinuity = pending.discontinuity;
            const auto sequence = CheckedAdd(mediaSequence, track.segments.size());
            if (!sequence) {
                return LineError(line.number, "the media sequence number overflows");
            }
            segment.sequence = *sequence;
            if (pending.range) {
                std::uint64_t offset = 0;
                if (pending.range->offset) {
                    offset = *pending.range->offset;
                } else if (previousRangeEnd && previousRangeEnd->first == segment.url) {
                    offset = previousRangeEnd->second;
                } else {
                    return LineError(line.number,
                                     "EXT-X-BYTERANGE without an offset must follow a byte range of the same file");
                }
                const auto end = CheckedAdd(offset, pending.range->length);
                if (!end) {
                    return LineError(line.number, "the byte range overflows");
                }
                segment.range = ByteRange{offset, pending.range->length};
                previousRangeEnd.emplace(segment.url, *end);
            } else {
                previousRangeEnd.reset();
            }
            segment.init = currentInit;
            segment.key = currentKey;
            if (currentKey) {
                segment.iv = currentIv.value_or(SequenceIv(segment.sequence));
            }
            if (track.segments.size() >= kMaxSegmentsPerTrack) {
                return LineError(line.number, "the playlist has more than " + std::to_string(kMaxSegmentsPerTrack) +
                                                  " segments");
            }
            track.segments.push_back(std::move(segment));
            pending = PendingSegment{};
            continue;
        }

        const auto [tag, value] = SplitTag(line.text);
        if (tag == "EXTINF") {
            if (pending.seconds) {
                return LineError(line.number, "EXTINF follows another without a segment address between");
            }
            pending.seconds = ParseNonNegativeDecimal(Trim(value.substr(0, value.find(','))));
            if (!pending.seconds) {
                return LineError(line.number, "EXTINF needs a duration in seconds");
            }
            pending.extinfLine = line.number;
        } else if (tag == "EXT-X-BYTERANGE") {
            pending.range = ParseRangeText(value);
            if (!pending.range) {
                return LineError(line.number, "EXT-X-BYTERANGE is not <length>[@<offset>]");
            }
        } else if (tag == "EXT-X-DISCONTINUITY") {
            pending.discontinuity = true;
        } else if (tag == "EXT-X-TARGETDURATION") {
            const auto seconds = ParseNonNegativeDecimal(value);
            if (!seconds || *seconds == 0) {
                return LineError(line.number, "EXT-X-TARGETDURATION is not a positive number");
            }
            track.targetSeconds = *seconds;
            targetSeen = true;
        } else if (tag == "EXT-X-MEDIA-SEQUENCE") {
            if (!track.segments.empty() || pending.seconds) {
                return LineError(line.number, "EXT-X-MEDIA-SEQUENCE must come before the first segment");
            }
            const auto sequence = ParseUnsigned(value);
            if (!sequence) {
                return LineError(line.number, "EXT-X-MEDIA-SEQUENCE is not a number");
            }
            mediaSequence = *sequence;
        } else if (tag == "EXT-X-ENDLIST") {
            endList = true;
        } else if (tag == "EXT-X-PLAYLIST-TYPE") {
            if (value != "VOD" && value != "EVENT") {
                return LineError(line.number, "EXT-X-PLAYLIST-TYPE is neither VOD nor EVENT");
            }
        } else if (tag == "EXT-X-MAP") {
            auto parsed = TagAttributes(line, value);
            if (!parsed) {
                return parsed.GetError();
            }
            InitSection init;
            const std::string uri = parsed.Value().Text("URI");
            auto url = uri.empty() ? std::nullopt : ResolveMediaUrl(baseUrl, uri);
            if (!url) {
                return LineError(line.number, "EXT-X-MAP needs an http or https URI");
            }
            init.url = std::move(*url);
            if (const std::string* rangeText = parsed.Value().Find("BYTERANGE")) {
                const auto range = ParseRangeText(*rangeText);
                if (!range || !CheckedAdd(range->offset.value_or(0), range->length)) {
                    return LineError(line.number, "the BYTERANGE of EXT-X-MAP is not <length>[@<offset>]");
                }
                init.range = ByteRange{range->offset.value_or(0), range->length};
            }
            track.inits.push_back(std::move(init));
            currentInit = track.inits.size() - 1;
        } else if (tag == "EXT-X-KEY") {
            auto parsed = TagAttributes(line, value);
            if (!parsed) {
                return parsed.GetError();
            }
            const AttributeList& attributes = parsed.Value();
            if (attributes.Text("METHOD") == "NONE") {
                currentKey.reset();
                currentIv.reset();
                continue;
            }
            if (auto system = ProtectionOf(attributes)) {
                AddDrmSystem(drm, std::move(*system));
                continue;
            }
            const std::string uri = attributes.Text("URI");
            EncryptionKey key;
            if (IsDataUri(uri)) {
                key.value = DecodeDataKey(uri);
                if (!key.value) {
                    return LineError(line.number, "the key's data: address does not hold 16 bytes in base64");
                }
            } else {
                // An empty reference would resolve to the playlist itself.
                auto url = uri.empty() ? std::nullopt : ResolveMediaUrl(baseUrl, uri);
                if (!url) {
                    return LineError(line.number, "EXT-X-KEY needs an http or https URI");
                }
                key.url = std::move(*url);
            }
            currentIv.reset();
            if (const std::string* iv = attributes.Find("IV")) {
                currentIv = ParseIv(*iv);
                if (!currentIv) {
                    return LineError(line.number, "the IV is not 0x followed by 32 hex digits");
                }
            }
            track.keys.push_back(std::move(key));
            currentKey = track.keys.size() - 1;
        }
    }
    if (pending.seconds) {
        return LineError(pending.extinfLine, "EXTINF is not followed by a segment address");
    }
    if (!targetSeen) {
        return base::MakeError(base::ErrorCode::Protocol, "the media playlist has no EXT-X-TARGETDURATION");
    }
    track.live = !endList;
    if (drm) {
        TrackSegments protectedTrack;
        protectedTrack.targetSeconds = track.targetSeconds;
        protectedTrack.live = track.live;
        protectedTrack.drm = std::move(drm);
        return protectedTrack;
    }
    return track;
}

}  // namespace

base::Result<HlsPlaylist> ParseHls(std::string_view text, std::string_view playlistUrl)
{
    if (text.size() > kMaxManifestBytes) {
        return base::MakeError(base::ErrorCode::Protocol, "the playlist is larger than " +
                                                              std::to_string(kMaxManifestBytes / (1024 * 1024)) + " MiB");
    }
    const auto baseUrl = ResolveMediaUrl(playlistUrl, "");
    if (!baseUrl) {
        return base::MakeError(base::ErrorCode::InvalidArgument, "the playlist's address is not an http or https address");
    }
    if (text.starts_with(kBom)) {
        text.remove_prefix(kBom.size());
    }
    const std::vector<Line> lines = SplitLines(text);
    if (lines.empty() || lines.front().text != "#EXTM3U") {
        return base::MakeError(base::ErrorCode::Protocol, "not an HLS playlist (no #EXTM3U first line)");
    }
    const auto kind = KindOf(lines);
    if (!kind) {
        return kind.GetError();
    }
    if (kind.Value() == PlaylistKind::Master) {
        auto offer = ParseMaster(lines, *baseUrl);
        if (!offer) {
            return offer.GetError();
        }
        return HlsPlaylist{std::move(offer).Value()};
    }
    auto track = ParseMedia(lines, *baseUrl);
    if (!track) {
        return track.GetError();
    }
    return HlsPlaylist{std::move(track).Value()};
}

}  // namespace ldm::media
