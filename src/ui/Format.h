#pragma once

#include "core/Snapshot.h"

#include <cstdint>
#include <optional>
#include <string>

// Text for the download list and dialogs. Pure functions, unit tested; no Win32.
namespace ldm::ui {

// "512 Bytes", "1.50 KB", "12.34 MB", "1.07 GB" (binary units).
[[nodiscard]] std::wstring FormatSize(std::int64_t bytes);
// Size column: the total when known, otherwise what arrived so far, otherwise empty.
[[nodiscard]] std::wstring FormatSizeColumn(const core::DownloadView& view);
// "1.25 MB/sec"; empty for zero.
[[nodiscard]] std::wstring FormatSpeed(std::int64_t bytesPerSecond);
// "45 sec", "3 min 05 sec", "2 hour(s) 10 min", "3 day(s) 4 hour(s)"; empty when unknown.
[[nodiscard]] std::wstring FormatTimeLeft(std::optional<std::int64_t> seconds);
// Status column: "45.23%" while downloading with a known size, otherwise the state in words.
// A recording's length: "4:05", or "1:02:03" from an hour on.
[[nodiscard]] std::wstring FormatRecordedLength(std::int64_t microseconds);
[[nodiscard]] std::wstring FormatStatus(const core::DownloadView& view);
// Percentage done, when the size is known.
[[nodiscard]] std::optional<double> ProgressFraction(const core::DownloadView& view);
// Local date and time in the user's short format, from Unix milliseconds; empty when absent.
[[nodiscard]] std::wstring FormatDateTime(std::optional<std::int64_t> unixMs);
// Resume capability for the progress window (checklist P-02).
[[nodiscard]] std::wstring FormatResumable(std::optional<bool> resumable);

}  // namespace ldm::ui
