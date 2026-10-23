#include "ui/Format.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace ldm::ui {

namespace {

std::wstring Printf(const wchar_t* format, double value, const wchar_t* unit)
{
    wchar_t buffer[64] = {};
    std::swprintf(buffer, std::size(buffer), format, value, unit);
    return buffer;
}

}  // namespace

std::wstring FormatSize(std::int64_t bytes)
{
    if (bytes < 0) {
        return {};
    }
    if (bytes < 1024) {
        return std::to_wstring(bytes) + L" Bytes";
    }
    constexpr std::array<const wchar_t*, 5> kUnits{L"KB", L"MB", L"GB", L"TB", L"PB"};
    double value = static_cast<double>(bytes) / 1024.0;
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < kUnits.size()) {
        value /= 1024.0;
        ++unit;
    }
    // Truncate rather than round, so 1023.999 KB never shows as "1024.00 KB".
    value = std::floor(value * 100.0) / 100.0;
    return Printf(L"%.2f %ls", value, kUnits[unit]);
}

std::wstring FormatSizeColumn(const core::DownloadView& view)
{
    if (view.download.totalSize) {
        return FormatSize(*view.download.totalSize);
    }
    if (view.receivedBytes > 0) {
        return FormatSize(view.receivedBytes);
    }
    return {};
}

std::wstring FormatSpeed(std::int64_t bytesPerSecond)
{
    if (bytesPerSecond <= 0) {
        return {};
    }
    return FormatSize(bytesPerSecond) + L"/sec";
}

std::wstring FormatTimeLeft(std::optional<std::int64_t> seconds)
{
    if (!seconds || *seconds < 0) {
        return {};
    }
    const std::int64_t s = *seconds;
    wchar_t buffer[64] = {};
    if (s < 60) {
        std::swprintf(buffer, std::size(buffer), L"%lld sec", static_cast<long long>(s));
    } else if (s < 3600) {
        std::swprintf(buffer, std::size(buffer), L"%lld min %02lld sec", static_cast<long long>(s / 60),
                      static_cast<long long>(s % 60));
    } else if (s < 86400) {
        std::swprintf(buffer, std::size(buffer), L"%lld hour(s) %lld min", static_cast<long long>(s / 3600),
                      static_cast<long long>((s % 3600) / 60));
    } else {
        std::swprintf(buffer, std::size(buffer), L"%lld day(s) %lld hour(s)", static_cast<long long>(s / 86400),
                      static_cast<long long>((s % 86400) / 3600));
    }
    return buffer;
}

std::optional<double> ProgressFraction(const core::DownloadView& view)
{
    const auto& total = view.download.totalSize;
    if (!total) {
        return std::nullopt;
    }
    if (*total == 0) {
        return 1.0;
    }
    return std::clamp(static_cast<double>(view.receivedBytes) / static_cast<double>(*total), 0.0, 1.0);
}

std::wstring FormatStatus(const core::DownloadView& view)
{
    using model::DownloadStatus;
    if (view.waitingForAddress && view.download.status != DownloadStatus::Stopping) {
        return L"Waiting for new address";
    }
    switch (view.download.status) {
    case DownloadStatus::Queued:
        return L"Queued";
    case DownloadStatus::Downloading: {
        if (view.retrying && view.connections.empty()) {
            return L"Retrying...";
        }
        if (const auto fraction = ProgressFraction(view)) {
            // Truncated so a download never shows 100.00% before it completes.
            const double percent = std::floor(*fraction * 10000.0) / 100.0;
            return Printf(L"%.2f%ls", std::min(percent, 99.99), L"%");
        }
        return view.receivedBytes > 0 ? L"Receiving data..." : L"Connecting...";
    }
    case DownloadStatus::Stopping:
        return L"Stopping...";
    case DownloadStatus::Paused: {
        if (view.waitingForQuota) {
            return L"Waiting for quota";
        }
        if (const auto fraction = ProgressFraction(view); fraction && *fraction > 0.0) {
            return Printf(L"%.2f%ls", std::floor(*fraction * 10000.0) / 100.0, L"% (paused)");
        }
        return L"Paused";
    }
    case DownloadStatus::Finalizing:
        return L"Finishing...";
    case DownloadStatus::Completed:
        return L"Complete";
    case DownloadStatus::Failed:
        return L"Error";
    case DownloadStatus::Deleting:
        return L"Deleting...";
    }
    return {};
}

std::wstring FormatDateTime(std::optional<std::int64_t> unixMs)
{
    if (!unixMs || *unixMs <= 0) {
        return {};
    }
    // Unix epoch to FILETIME (100 ns intervals since 1601).
    constexpr std::int64_t kEpochDifference = 116444736000000000LL;
    const std::int64_t ticks = *unixMs * 10000 + kEpochDifference;
    FILETIME utc{static_cast<DWORD>(ticks & 0xFFFFFFFF), static_cast<DWORD>(ticks >> 32)};
    FILETIME local{};
    SYSTEMTIME time{};
    if (!::FileTimeToLocalFileTime(&utc, &local) || !::FileTimeToSystemTime(&local, &time)) {
        return {};
    }
    wchar_t date[64] = {};
    wchar_t clock[64] = {};
    ::GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &time, nullptr, date, static_cast<int>(std::size(date)), nullptr);
    ::GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &time, nullptr, clock, static_cast<int>(std::size(clock)));
    return std::wstring(date) + L" " + clock;
}

std::wstring FormatResumable(std::optional<bool> resumable)
{
    if (!resumable) {
        return L"Unknown";
    }
    return *resumable ? L"Yes" : L"No";
}

}  // namespace ldm::ui
