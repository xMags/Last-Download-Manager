#pragma once

#include "base/Result.h"
#include "model/GrabberProject.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// A Grabber project's definition as stored (M4 design section 6.1): one JSON document, read back
// as untrusted input because a damaged database or a hand-edited file must not crash a crawl.
namespace ldm::grabber {

inline constexpr std::size_t kMaxDefinitionBytes = 256 * 1024;

[[nodiscard]] std::string DefinitionToJson(const model::GrabberProjectDefinition& definition);
// Rejects anything malformed: wrong types, a start page that is not an http or https address,
// depths, concurrency or sizes out of range, too many or too long masks, a relative folder.
// Missing fields take their defaults.
[[nodiscard]] base::Result<model::GrabberProjectDefinition> ParseDefinition(std::string_view json);

// The filters and save options a template stands for (checklist G-01), applied to `definition`.
// Custom changes nothing. The start page, the save place and the concurrency stay as they are.
[[nodiscard]] model::GrabberProjectDefinition ApplyTemplate(model::GrabberProjectDefinition definition,
                                                            model::GrabberTemplate templateKind);

// The masks of a file type group, as the templates and the Grabber window's filters use them.
[[nodiscard]] std::vector<std::string> PictureMasks();
[[nodiscard]] std::vector<std::string> AudioMasks();
[[nodiscard]] std::vector<std::string> VideoMasks();

}  // namespace ldm::grabber
