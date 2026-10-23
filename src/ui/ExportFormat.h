#pragma once

#include "core/Snapshot.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Tasks > Export and Import (checklist A-11).
namespace ldm::ui {

struct ImportedDownload {
    std::string url;
    std::optional<std::string> fileName;
    std::string description;
    std::optional<std::string> referer;
};

struct ImportParse {
    std::vector<ImportedDownload> downloads;
    std::optional<std::string> error;  // the whole file is rejected
};

// LDM export file: JSON {"format":"ldm-export","version":1,"downloads":[...]}. Holds addresses and
// names only; never passwords or local state.
[[nodiscard]] std::string ExportLdm(std::span<const core::DownloadView* const> downloads);
// Validates the file as untrusted input: structure, address schemes, name and size limits.
[[nodiscard]] ImportParse ParseLdmExport(std::string_view text);

// Plain text: one address per line.
[[nodiscard]] std::string ExportText(std::span<const core::DownloadView* const> downloads);

inline constexpr size_t kMaxImportBytes = 16 * 1024 * 1024;
inline constexpr size_t kMaxImportEntries = 100'000;

}  // namespace ldm::ui
