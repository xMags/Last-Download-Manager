#pragma once

#include <windows.h>

#include <optional>
#include <string>

namespace ldm::ui {

// The Windows folder picker, starting in `initial` when it exists.
[[nodiscard]] std::optional<std::wstring> PickFolder(HWND owner, const std::wstring& initial);

// The Windows open and save file dialogs with one filter, e.g. L"Text files", L"*.txt".
[[nodiscard]] std::optional<std::wstring> PickOpenFile(HWND owner, const wchar_t* filterName, const wchar_t* pattern);
[[nodiscard]] std::optional<std::wstring> PickSaveFile(HWND owner, const wchar_t* filterName, const wchar_t* pattern,
                                                       const wchar_t* defaultExtension, const std::wstring& suggestedName);

}  // namespace ldm::ui
