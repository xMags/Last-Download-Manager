#include "platform/win/RunKeyStartup.h"

#include <windows.h>

#include <string>

namespace ldm::platform {

namespace {

constexpr wchar_t kRunKey[] = LR"(Software\Microsoft\Windows\CurrentVersion\Run)";
constexpr wchar_t kValueName[] = L"Last Download Manager";

std::wstring ExecutablePath()
{
    std::wstring path(MAX_PATH, L'\0');
    while (true) {
        const DWORD length = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return {};
        }
        if (length < path.size()) {
            path.resize(length);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

}  // namespace

bool RunKeyStartup::IsEnabled()
{
    DWORD size = 0;
    return ::RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValueName, RRF_RT_REG_SZ, nullptr, nullptr, &size) == ERROR_SUCCESS;
}

bool RunKeyStartup::SetEnabled(bool enabled)
{
    HKEY key = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    LSTATUS status = ERROR_SUCCESS;
    if (enabled) {
        const std::wstring command = L"\"" + ExecutablePath() + L"\" --tray";
        status = ::RegSetValueExW(key, kValueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                                  static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        status = ::RegDeleteValueW(key, kValueName);
        if (status == ERROR_FILE_NOT_FOUND) {
            status = ERROR_SUCCESS;
        }
    }
    ::RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

}  // namespace ldm::platform
