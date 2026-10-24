#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// Playlist files that list links (checklist A-12): a plain M3U or M3U8 file, as a radio station or
// a music site offers it, names files to download one per line. Not an HLS playlist, which lists
// the segments of one stream. Pure: the caller reads the file.
namespace ldm::media {

// At most this many links are taken from one file.
inline constexpr size_t kMaxM3uLinks = 10'000;

struct M3uEntry {
    std::string url;    // absolute: http, https, ftp or ftps with a host
    std::string title;  // from the #EXTINF line before it; may be empty
};

// The links in a plain M3U or M3U8 file fetched from `baseUrl`, relative ones resolved against
// it, in the file's order without repeats. Empty for an HLS playlist (any #EXT-X- tag), as its
// lines are segments of one stream. A UTF-8 byte order mark and CRLF line ends are fine; Windows
// paths and other schemes are left out.
[[nodiscard]] std::vector<M3uEntry> ParseM3uLinks(std::string_view text, std::string_view baseUrl);

}  // namespace ldm::media
