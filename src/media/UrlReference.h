#pragma once

#include <optional>
#include <string>
#include <string_view>

// Addresses in manifests are usually relative to the manifest (or to a DASH BaseURL), so every
// segment, key and playlist address goes through these before LDM can request it.
namespace ldm::media {

// Resolves `reference` against `base` exactly as RFC 3986 section 5.2 describes (strict mode,
// with dot-segment removal). Nothing when `base` has no scheme. Neither input is validated beyond
// the RFC's own split of an address into its parts; ResolveMediaUrl adds LDM's rules.
[[nodiscard]] std::optional<std::string> ResolveReference(std::string_view base, std::string_view reference);

// An address a manifest may point LDM at: the resolved address must be http or https with a host.
// The scheme is lower-cased, the fragment dropped (never part of a request) and spaces are
// percent-encoded, as some playlists carry them unencoded. Nothing for any other scheme (file,
// data, javascript, ...), a missing host, or control characters in either input.
[[nodiscard]] std::optional<std::string> ResolveMediaUrl(std::string_view base, std::string_view reference);

// The same for a link LDM may add as a download (a playlist file's entries): http, https, ftp or
// ftps with a host.
[[nodiscard]] std::optional<std::string> ResolveLinkUrl(std::string_view base, std::string_view reference);

}  // namespace ldm::media
