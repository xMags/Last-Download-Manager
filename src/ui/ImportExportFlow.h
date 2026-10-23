#pragma once

#include "core/Snapshot.h"
#include "ui/AddFlow.h"
#include "ui/Theme.h"

#include <windows.h>

#include <functional>
#include <vector>

namespace ldm::ui {

// Tasks > Export and Tasks > Import (checklist A-11). Exports the selected downloads, or every
// download when nothing is selected. Files are small and the user waits on a modal file dialog
// anyway, so reading and writing happen on the UI thread.
class ImportExportFlow {
public:
    struct Context {
        HWND owner = nullptr;
        const Theme* theme = nullptr;
        AddFlow* adds = nullptr;
        std::function<std::shared_ptr<const core::Snapshot>()> snapshot;
        std::function<std::vector<const core::DownloadView*>()> selection;
    };

    explicit ImportExportFlow(Context context);

    void ExportLdmFile();
    void ExportTextFile();
    void ImportLdmFile();
    void ImportTextFile();

private:
    // The selection, or every download; nothing when the list is empty.
    [[nodiscard]] std::vector<const core::DownloadView*> ExportSet(std::shared_ptr<const core::Snapshot>& keepAlive) const;
    void ShowError(const std::wstring& message) const;

    Context context_;
};

}  // namespace ldm::ui
