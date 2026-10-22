#include "ui/FileDialogs.h"

#include <shobjidl.h>
#include <wrl/client.h>

namespace ldm::ui {

namespace {

using Microsoft::WRL::ComPtr;

std::optional<std::wstring> ResultPath(IFileDialog& dialog)
{
    ComPtr<IShellItem> item;
    if (FAILED(dialog.GetResult(&item))) {
        return std::nullopt;
    }
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        return std::nullopt;
    }
    std::wstring result(path);
    ::CoTaskMemFree(path);
    return result;
}

void SetInitialFolder(IFileDialog& dialog, const std::wstring& folder)
{
    if (folder.empty()) {
        return;
    }
    ComPtr<IShellItem> item;
    if (SUCCEEDED(::SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
        dialog.SetFolder(item.Get());
    }
}

}  // namespace

std::optional<std::wstring> PickFolder(HWND owner, const std::wstring& initial)
{
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return std::nullopt;
    }
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    SetInitialFolder(*dialog.Get(), initial);
    if (FAILED(dialog->Show(owner))) {
        return std::nullopt;
    }
    return ResultPath(*dialog.Get());
}

std::optional<std::wstring> PickOpenFile(HWND owner, const wchar_t* filterName, const wchar_t* pattern)
{
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return std::nullopt;
    }
    const COMDLG_FILTERSPEC filters[] = {{filterName, pattern}, {L"All files", L"*.*"}};
    dialog->SetFileTypes(2, filters);
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    if (FAILED(dialog->Show(owner))) {
        return std::nullopt;
    }
    return ResultPath(*dialog.Get());
}

std::optional<std::wstring> PickSaveFile(HWND owner, const wchar_t* filterName, const wchar_t* pattern,
                                         const wchar_t* defaultExtension, const std::wstring& suggestedName)
{
    ComPtr<IFileSaveDialog> dialog;
    if (FAILED(::CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return std::nullopt;
    }
    const COMDLG_FILTERSPEC filters[] = {{filterName, pattern}};
    dialog->SetFileTypes(1, filters);
    dialog->SetDefaultExtension(defaultExtension);
    dialog->SetFileName(suggestedName.c_str());
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);
    if (FAILED(dialog->Show(owner))) {
        return std::nullopt;
    }
    return ResultPath(*dialog.Get());
}

}  // namespace ldm::ui
