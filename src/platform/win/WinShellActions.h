#pragma once

#include "ui/ShellActions.h"

namespace ldm::platform {

class WinShellActions final : public ui::ShellActions {
public:
    bool Open(HWND owner, const std::filesystem::path& file) override;
    bool OpenWith(HWND owner, const std::filesystem::path& file) override;
    bool ShowInFolder(const std::filesystem::path& file) override;
    bool OpenUrl(HWND owner, const std::wstring& url) override;
    bool MoveItem(HWND owner, const std::filesystem::path& from, const std::filesystem::path& to) override;
};

}  // namespace ldm::platform
