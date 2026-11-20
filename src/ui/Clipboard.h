#pragma once

#include <windows.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace ldm::ui {

// The clipboard's Unicode text, up to `maxChars` characters; empty when there is none, it is
// locked by another program, or it is longer than the limit (huge texts are never URLs worth
// offering). UI thread only.
[[nodiscard]] std::wstring ReadClipboardText(HWND owner, size_t maxChars);

// Puts `text` on the clipboard; false when the clipboard is locked by another program. UI thread only.
bool WriteClipboardText(HWND owner, std::wstring_view text);

}  // namespace ldm::ui
