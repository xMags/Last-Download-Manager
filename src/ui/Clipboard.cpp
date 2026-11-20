#include "ui/Clipboard.h"

#include <algorithm>
#include <cwchar>

namespace ldm::ui {

std::wstring ReadClipboardText(HWND owner, size_t maxChars)
{
    if (!::IsClipboardFormatAvailable(CF_UNICODETEXT) || !::OpenClipboard(owner)) {
        return {};
    }
    std::wstring text;
    if (const HANDLE data = ::GetClipboardData(CF_UNICODETEXT); data != nullptr) {
        const SIZE_T bytes = ::GlobalSize(data);
        if (const auto* chars = static_cast<const wchar_t*>(::GlobalLock(data)); chars != nullptr) {
            // Bounded by the allocation, so a clipboard without a terminator cannot be overread.
            const size_t capacity = bytes / sizeof(wchar_t);
            const size_t length = ::wcsnlen(chars, std::min(capacity, maxChars + 1));
            if (length <= maxChars) {
                text.assign(chars, length);
            }
            ::GlobalUnlock(data);
        }
    }
    ::CloseClipboard();
    return text;
}

bool WriteClipboardText(HWND owner, std::wstring_view text)
{
    if (!::OpenClipboard(owner)) {
        return false;
    }
    bool written = false;
    if (::EmptyClipboard()) {
        const HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
        if (memory != nullptr) {
            if (auto* chars = static_cast<wchar_t*>(::GlobalLock(memory)); chars != nullptr) {
                std::copy(text.begin(), text.end(), chars);
                chars[text.size()] = L'\0';
                ::GlobalUnlock(memory);
                // On success the clipboard owns the memory.
                written = ::SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
            }
            if (!written) {
                ::GlobalFree(memory);
            }
        }
    }
    ::CloseClipboard();
    return written;
}

}  // namespace ldm::ui
