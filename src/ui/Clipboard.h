#pragma once

#include <windows.h>

#include <cstddef>
#include <string>

namespace ldm::ui {

// The clipboard's Unicode text, up to `maxChars` characters; empty when there is none, it is
// locked by another program, or it is longer than the limit (huge texts are never URLs worth
// offering). UI thread only.
[[nodiscard]] std::wstring ReadClipboardText(HWND owner, size_t maxChars);

}  // namespace ldm::ui
