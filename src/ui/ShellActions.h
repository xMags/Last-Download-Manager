#pragma once

#include <windows.h>

#include <filesystem>
#include <string>

namespace ldm::ui {

// Explorer integration the UI needs (checklist V-03). Implemented in platform/win.
class ShellActions {
public:
    virtual ~ShellActions() = default;
    // Each returns false when Windows refused; the caller tells the user.
    virtual bool Open(HWND owner, const std::filesystem::path& file) = 0;
    virtual bool OpenWith(HWND owner, const std::filesystem::path& file) = 0;
    // Opens the folder in Explorer with the file selected, or just the folder when the file is gone.
    virtual bool ShowInFolder(const std::filesystem::path& file) = 0;
    virtual bool OpenUrl(HWND owner, const std::wstring& url) = 0;
    // Moves or renames a file with Explorer's own progress window for slow moves. Fails rather
    // than replacing an existing file.
    virtual bool MoveItem(HWND owner, const std::filesystem::path& from, const std::filesystem::path& to) = 0;
};

}  // namespace ldm::ui
