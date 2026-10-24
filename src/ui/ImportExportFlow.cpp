#include "ui/ImportExportFlow.h"

#include "base/Utf.h"
#include "core/Url.h"
#include "ui/ExportFormat.h"
#include "ui/FileDialogs.h"
#include "ui/SmallFile.h"
#include "ui/dialogs/MessageDialog.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace ldm::ui {

namespace {

// Writes next to the target, then replaces it, so a failed export never leaves half a file.
bool WriteFileReplacing(const std::filesystem::path& path, std::string_view bytes)
{
    std::filesystem::path temporary = path;
    temporary += L".ldmtmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            return false;
        }
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        file.flush();
        if (!file) {
            file.close();
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return false;
        }
    }
    if (!::MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
    return true;
}

// Text files from Notepad and other tools: UTF-8 with or without a byte order mark, or UTF-16 LE.
std::string DecodeText(const std::string& bytes)
{
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE) {
        std::wstring wide((bytes.size() - 2) / sizeof(wchar_t), L'\0');
        std::memcpy(wide.data(), bytes.data() + 2, wide.size() * sizeof(wchar_t));
        return base::ToUtf8(wide);
    }
    if (bytes.size() >= 3 && bytes.compare(0, 3, "\xEF\xBB\xBF") == 0) {
        return bytes.substr(3);
    }
    return bytes;
}

}  // namespace

ImportExportFlow::ImportExportFlow(Context context) : context_(std::move(context)) {}

void ImportExportFlow::ShowError(const std::wstring& message) const
{
    (void)ShowMessage(context_.owner, *context_.theme, MessageRequest{message, MessageKind::Warning});
}

std::vector<const core::DownloadView*> ImportExportFlow::ExportSet(std::shared_ptr<const core::Snapshot>& keepAlive) const
{
    keepAlive = context_.snapshot();
    std::vector<const core::DownloadView*> views = context_.selection();
    if (views.empty() && keepAlive) {
        for (const auto& view : keepAlive->downloads) {
            views.push_back(view.get());
        }
    }
    return views;
}

void ImportExportFlow::ExportLdmFile()
{
    std::shared_ptr<const core::Snapshot> keepAlive;
    const auto views = ExportSet(keepAlive);
    if (views.empty()) {
        ShowError(L"There are no downloads to export.");
        return;
    }
    const auto path = PickSaveFile(context_.owner, L"LDM export files", L"*.ldm.json", L"json", L"downloads.ldm.json");
    if (path && !WriteFileReplacing(*path, ExportLdm(views))) {
        ShowError(L"The export file could not be written.");
    }
}

void ImportExportFlow::ExportTextFile()
{
    std::shared_ptr<const core::Snapshot> keepAlive;
    const auto views = ExportSet(keepAlive);
    if (views.empty()) {
        ShowError(L"There are no downloads to export.");
        return;
    }
    const auto path = PickSaveFile(context_.owner, L"Text files", L"*.txt", L"txt", L"downloads.txt");
    if (path && !WriteFileReplacing(*path, ExportText(views))) {
        ShowError(L"The text file could not be written.");
    }
}

void ImportExportFlow::ImportLdmFile()
{
    const auto path = PickOpenFile(context_.owner, L"LDM export files", L"*.ldm.json;*.json");
    if (!path) {
        return;
    }
    const auto bytes = ReadSmallFile(*path, kMaxImportBytes);
    if (!bytes) {
        ShowError(L"The file could not be read, or it is too large to be an LDM export file.");
        return;
    }
    const ImportParse parse = ParseLdmExport(*bytes);
    if (parse.error) {
        ShowError(base::ToUtf16(*parse.error));
        return;
    }
    if (parse.downloads.empty()) {
        ShowError(L"The export file holds no downloads.");
        return;
    }
    std::vector<ImportedEntry> entries;
    entries.reserve(parse.downloads.size());
    for (const ImportedDownload& download : parse.downloads) {
        entries.push_back(ImportedEntry{download.url, download.fileName, download.description, download.referer});
    }
    context_.adds->AddImported(entries);
    const std::wstring message = std::to_wstring(entries.size()) +
                                 (entries.size() == 1 ? L" download was added, stopped." : L" downloads were added, stopped.");
    (void)ShowMessage(context_.owner, *context_.theme, MessageRequest{message, MessageKind::Information});
}

void ImportExportFlow::ImportTextFile()
{
    const auto path = PickOpenFile(context_.owner, L"Text files", L"*.txt");
    if (!path) {
        return;
    }
    const auto bytes = ReadSmallFile(*path, kMaxImportBytes);
    if (!bytes) {
        ShowError(L"The file could not be read, or it is too large.");
        return;
    }
    std::vector<std::string> urls = core::ExtractUrls(DecodeText(*bytes));
    if (urls.empty()) {
        ShowError(L"The file holds no download addresses.");
        return;
    }
    context_.adds->AddList(std::move(urls), L"Import from text file");
}

}  // namespace ldm::ui
