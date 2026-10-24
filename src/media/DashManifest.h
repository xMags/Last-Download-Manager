#pragma once

#include "base/Result.h"
#include "media/MediaModel.h"
#include "media/XmlElement.h"

#include <chrono>
#include <string_view>

// DASH manifests (MPD, ISO/IEC 23009-1, checklist B-12).
namespace ldm::media {

// Reads an MPD LDM fetched from `mpdUrl` (the address after redirects; http or https), given as
// the root element a platform XML reader produced. An MPD describes every segment itself, so each
// variant and rendition comes back with its TrackSegments already filled in.
//
// Covered: SegmentTemplate ($RepresentationID$, $Number$, $Bandwidth$, $Time$, with %0<width>d),
// SegmentTimeline with repeats (including r="-1"), SegmentList, BaseURL at every level, and
// inheritance of segment information from Period to AdaptationSet to Representation.
// SegmentBase (and a representation with no segment information) is one file downloaded whole:
// its init section and index sit inside that file, so the whole file is already a valid one.
// A dynamic (live) MPD yields the segments available at `now`, inside its time-shift window;
// with no window, its most recent kMaxSegmentsPerTrack segments. ContentProtection marks DRM.
//
// Several periods are joined in order when every period has the same representation ids (a long
// video cut into parts); anything else, typically inserted ads, is refused as unsupported rather
// than guessed at. Trick-mode adaptation sets and image (thumbnail) tracks are left out.
[[nodiscard]] base::Result<MediaOffer> ParseMpd(const XmlElement& mpd, std::string_view mpdUrl,
                                                std::chrono::system_clock::time_point now);

}  // namespace ldm::media
