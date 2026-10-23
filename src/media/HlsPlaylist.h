#pragma once

#include "base/Result.h"
#include "media/MediaModel.h"

#include <string_view>
#include <variant>

// HLS playlists (RFC 8216, checklist B-12).
namespace ldm::media {

// A master playlist becomes the offer; a media playlist becomes one track's download list.
using HlsPlaylist = std::variant<MediaOffer, TrackSegments>;

// Reads a playlist LDM fetched from `playlistUrl` (the address after redirects, which relative
// addresses resolve against; it must be http or https). Strict where LDM relies on a value
// (segment addresses, durations, byte ranges, keys) and tolerant of everything else, as the RFC
// asks of clients: unknown tags are ignored. Any error names the line.
//
// Covered: variants (EXT-X-STREAM-INF), audio and subtitle renditions (EXT-X-MEDIA), fMP4 init
// sections (EXT-X-MAP), byte ranges including ranges that continue the previous one, discontinuities,
// AES-128 with explicit or sequence-number IVs and data: keys, live playlists (no EXT-X-ENDLIST).
// SAMPLE-AES and key formats other than "identity" mark DRM (EXT-X-KEY, EXT-X-SESSION-KEY).
// I-frame playlists, alternative video renditions and closed captions are left out: closed
// captions travel inside the video, the others are not something to download.
[[nodiscard]] base::Result<HlsPlaylist> ParseHls(std::string_view text, std::string_view playlistUrl);

}  // namespace ldm::media
