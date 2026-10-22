#include "platform/win/WinShellActions.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <filesystem>
#include <memory>

#include <wrl/client.h>

namespace ldm::platform {

namespace {

struct ItemIdListDeleter {
    using pointer = PIDLIST_ABSOLUTE;  // keeps the __unaligned qualifier of the shell's pointer type
    void operator()(PIDLIST_ABSOLUTE list) const noexcept { ::ILFree(list); }
};
using UniqueItemIdList = std::unique_ptr<ITEMIDLIST_ABSOLUTE, ItemIdListDeleter>;

UniqueItemIdList ParseName(const std::filesystem::path& path)
{
    PIDLIST_ABSOLUTE list = nullptr;
    if (FAILED(::SHParseDisplayName(path.c_str(), nullptr, &list, 0, nullptr))) {
        return nullptr;
    }
    return UniqueItemIdList(list);
}

}  // namespace

bool WinShellActions::Open(HWND owner, const std::filesystem::path& file)
{
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.hwnd = owner;
    info.lpVerb = nullptr;  // the default verb, as a double-click in Explorer
    info.lpFile = file.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (::ShellExecuteExW(&info)) {
        return true;
    }
    // No association: offer the Open with dialog instead of failing silently.
    return GetLastError() == ERROR_NO_ASSOCIATION && OpenWith(owner, file);
}

bool WinShellActions::OpenWith(HWND owner, const std::filesystem::path& file)
{
    OPENASINFO info{};
    info.pcszFile = file.c_str();
    info.oaifInFlags = OAIF_ALLOW_REGISTRATION | OAIF_EXEC;
    return SUCCEEDED(::SHOpenWithDialog(owner, &info));
}

bool WinShellActions::ShowInFolder(const std::filesystem::path& file)
{
    std::error_code error;
    if (!std::filesystem::exists(file, error)) {
        const auto folder = ParseName(file.parent_path());
        return folder && SUCCEEDED(::SHOpenFolderAndSelectItems(folder.get(), 0, nullptr, 0));
    }
    const auto item = ParseName(file);
    return item && SUCCEEDED(::SHOpenFolderAndSelectItems(item.get(), 0, nullptr, 0));
}

bool WinShellActions::MoveItem(HWND owner, const std::filesystem::path& from, const std::filesystem::path& to)
{
    std::error_code error;
    if (std::filesystem::exists(to, error)) {
        return false;
    }
    std::filesystem::create_directories(to.parent_path(), error);
    Microsoft::WRL::ComPtr<IFileOperation> operation;
    if (FAILED(::CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&operation)))) {
        return false;
    }
    operation->SetOwnerWindow(owner);
    operation->SetOperationFlags(FOF_NOCONFIRMMKDIR | FOF_NOCONFIRMATION | FOF_NO_CONNECTED_ELEMENTS | FOFX_NOCOPYHOOKS);
    Microsoft::WRL::ComPtr<IShellItem> source;
    Microsoft::WRL::ComPtr<IShellItem> folder;
    if (FAILED(::SHCreateItemFromParsingName(from.c_str(), nullptr, IID_PPV_ARGS(&source))) ||
        FAILED(::SHCreateItemFromParsingName(to.parent_path().c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
        return false;
    }
    if (FAILED(operation->MoveItem(source.Get(), folder.Get(), to.filename().c_str(), nullptr)) ||
        FAILED(operation->PerformOperations())) {
        return false;
    }
    BOOL aborted = FALSE;
    operation->GetAnyOperationsAborted(&aborted);
    return !aborted && std::filesystem::exists(to, error);
}

bool WinShellActions::OpenUrl(HWND owner, const std::wstring& url)
{
    if (url.rfind(L"https://", 0) != 0) {
        return false;  // only web pages, never local programs
    }
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_NOASYNC;
    info.hwnd = owner;
    info.lpVerb = L"open";
    info.lpFile = url.c_str();
    info.nShow = SW_SHOWNORMAL;
    return ::ShellExecuteExW(&info) != FALSE;
}

}  // namespace ldm::platform
