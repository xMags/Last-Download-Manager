#include "ui/ExportFormat.h"

#include "core/FileNames.h"
#include "core/Url.h"

#include <nlohmann/json.hpp>

namespace ldm::ui {

namespace {

constexpr std::string_view kFormat = "ldm-export";
constexpr int kVersion = 1;
constexpr size_t kMaxFieldBytes = 32 * 1024;

std::optional<std::string> Field(const nlohmann::json& entry, const char* key, bool& valid)
{
    const auto it = entry.find(key);
    if (it == entry.end() || it->is_null()) {
        return std::nullopt;
    }
    if (!it->is_string() || it->get_ref<const std::string&>().size() > kMaxFieldBytes) {
        valid = false;
        return std::nullopt;
    }
    return it->get<std::string>();
}

}  // namespace

std::string ExportLdm(std::span<const core::DownloadView* const> downloads)
{
    nlohmann::json list = nlohmann::json::array();
    for (const core::DownloadView* view : downloads) {
        const model::Download& d = view->download;
        nlohmann::json entry = {{"url", d.url}, {"fileName", d.fileName}};
        if (!d.description.empty()) {
            entry["description"] = d.description;
        }
        if (d.referer) {
            entry["referer"] = *d.referer;
        }
        list.push_back(std::move(entry));
    }
    const nlohmann::json document = {{"format", kFormat}, {"version", kVersion}, {"downloads", std::move(list)}};
    return document.dump(2);
}

ImportParse ParseLdmExport(std::string_view text)
{
    ImportParse parse;
    if (text.size() > kMaxImportBytes) {
        parse.error = "The file is too large to be an LDM export file.";
        return parse;
    }
    const nlohmann::json document = nlohmann::json::parse(text, nullptr, false);
    if (document.is_discarded() || !document.is_object() || document.value("format", std::string()) != kFormat) {
        parse.error = "The file is not an LDM export file.";
        return parse;
    }
    const auto version = document.find("version");
    if (version == document.end() || !version->is_number_integer() || version->get<int>() != kVersion) {
        parse.error = "The export file was made by a newer version of Last Download Manager.";
        return parse;
    }
    const auto downloads = document.find("downloads");
    if (downloads == document.end() || !downloads->is_array() || downloads->size() > kMaxImportEntries) {
        parse.error = "The export file has no valid download list.";
        return parse;
    }
    for (const nlohmann::json& entry : *downloads) {
        bool valid = entry.is_object();
        ImportedDownload download;
        if (valid) {
            download.url = Field(entry, "url", valid).value_or("");
            download.fileName = Field(entry, "fileName", valid);
            download.description = Field(entry, "description", valid).value_or("");
            download.referer = Field(entry, "referer", valid);
        }
        if (!valid || !core::ParseUrl(download.url) ||
            (download.fileName && core::SanitizeFileName(*download.fileName) != *download.fileName) ||
            (download.referer && !core::ParseUrl(*download.referer))) {
            parse.downloads.clear();
            parse.error = "The export file contains an invalid entry; nothing was imported.";
            return parse;
        }
        parse.downloads.push_back(std::move(download));
    }
    return parse;
}

std::string ExportText(std::span<const core::DownloadView* const> downloads)
{
    std::string text;
    for (const core::DownloadView* view : downloads) {
        text += view->download.url;
        text += "\r\n";
    }
    return text;
}

}  // namespace ldm::ui
