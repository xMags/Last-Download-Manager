#include "platform/win/BrowserHostRegistration.h"

#include "base/Utf.h"
#include "browser/HostManifest.h"

#include <windows.h>

#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>

namespace ldm::platform {

namespace {

struct BrowserKey {
    const wchar_t* registryPath;  // under HKCU, without the host name
    browser::BrowserFamily family;
};

constexpr BrowserKey kBrowserKeys[] = {
    {L"Software\\Google\\Chrome\\NativeMessagingHosts\\", browser::BrowserFamily::Chromium},
    {L"Software\\Microsoft\\Edge\\NativeMessagingHosts\\", browser::BrowserFamily::Chromium},
    {L"Software\\Mozilla\\NativeMessagingHosts\\", browser::BrowserFamily::Firefox},
};

std::filesystem::path ManifestPath(const std::filesystem::path& folder, browser::BrowserFamily family)
{
    return folder / (family == browser::BrowserFamily::Firefox ? L"firefox-host.json" : L"chromium-host.json");
}

std::optional<std::string> ReadFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// Writes through a temporary file and a rename, so a browser never reads half a manifest.
bool WriteFileIfChanged(const std::filesystem::path& path, const std::string& content)
{
    if (ReadFile(path) == content) {
        return true;
    }
    std::filesystem::path temporary = path;
    temporary += L".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file.write(content.data(), static_cast<std::streamsize>(content.size()))) {
            return false;
        }
    }
    return ::MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

std::optional<std::wstring> ReadDefaultValue(HKEY key)
{
    DWORD type = 0;
    DWORD bytes = 0;
    if (::RegQueryValueExW(key, nullptr, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS || type != REG_SZ) {
        return std::nullopt;
    }
    std::wstring value(bytes / sizeof(wchar_t) + 1, L'\0');
    if (::RegQueryValueExW(key, nullptr, nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &bytes) !=  // registry API takes bytes
            ERROR_SUCCESS ||
        type != REG_SZ) {
        return std::nullopt;
    }
    value.resize(::wcsnlen(value.c_str(), value.size()));
    return value;
}

bool PointKeyAt(const std::wstring& subKey, const std::wstring& manifest)
{
    HKEY key = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, subKey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                          KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    bool ok = true;
    if (ReadDefaultValue(key) != manifest) {
        const auto bytes = static_cast<DWORD>((manifest.size() + 1) * sizeof(wchar_t));
        ok = ::RegSetValueExW(key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(manifest.c_str()), bytes) ==  // registry API takes bytes
             ERROR_SUCCESS;
    }
    ::RegCloseKey(key);
    return ok;
}

}  // namespace

bool IsProcessElevated()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const BOOL ok = ::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    ::CloseHandle(token);
    return ok != FALSE && elevation.TokenIsElevated != 0;
}

bool RegisterBrowserHosts(const std::filesystem::path& manifestFolder, const std::filesystem::path& hostExecutable)
{
    std::error_code error;
    std::filesystem::create_directories(manifestFolder, error);
    if (error) {
        return false;
    }
    bool ok = true;
    for (const auto family : {browser::BrowserFamily::Chromium, browser::BrowserFamily::Firefox}) {
        ok = WriteFileIfChanged(ManifestPath(manifestFolder, family), browser::NativeHostManifest(family, hostExecutable)) && ok;
    }
    const std::wstring hostName = base::ToUtf16(browser::kHostName);
    for (const BrowserKey& browserKey : kBrowserKeys) {
        ok = PointKeyAt(browserKey.registryPath + hostName, ManifestPath(manifestFolder, browserKey.family).wstring()) && ok;
    }
    return ok;
}

}  // namespace ldm::platform
