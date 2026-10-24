#include "grabber/ProjectDefinition.h"

#include "core/Url.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace ldm::grabber {

namespace {

constexpr int kVersion = 1;

// Reads fields of one JSON object; the first problem marks the whole definition invalid.
class Reader {
public:
    Reader(const nlohmann::json& object, bool& valid) : object_(object), valid_(valid)
    {
        if (!object_.is_object()) {
            valid_ = false;
        }
    }

    void Bool(const char* key, bool& out)
    {
        const nlohmann::json* value = Find(key);
        if (value == nullptr) {
            return;
        }
        if (!value->is_boolean()) {
            valid_ = false;
            return;
        }
        out = value->get<bool>();
    }

    void Int(const char* key, int& out, int min, int max)
    {
        const nlohmann::json* value = Find(key);
        if (value == nullptr) {
            return;
        }
        if (!value->is_number_integer() || value->get<std::int64_t>() < min || value->get<std::int64_t>() > max) {
            valid_ = false;
            return;
        }
        out = value->get<int>();
    }

    // A number, or null for nothing.
    void OptionalInt64(const char* key, std::optional<std::int64_t>& out, std::int64_t min, std::int64_t max)
    {
        const nlohmann::json* value = Find(key);
        if (value == nullptr) {
            return;
        }
        if (value->is_null()) {
            out.reset();
            return;
        }
        if (!value->is_number_integer() || value->get<std::int64_t>() < min || value->get<std::int64_t>() > max) {
            valid_ = false;
            return;
        }
        out = value->get<std::int64_t>();
    }

    void String(const char* key, std::string& out, std::size_t maxLength)
    {
        const nlohmann::json* value = Find(key);
        if (value == nullptr) {
            return;
        }
        if (!value->is_string() || value->get_ref<const std::string&>().size() > maxLength) {
            valid_ = false;
            return;
        }
        out = value->get<std::string>();
    }

    void Masks(const char* key, std::vector<std::string>& out)
    {
        const nlohmann::json* value = Find(key);
        if (value == nullptr) {
            return;
        }
        if (!value->is_array() || value->size() > model::kMaxGrabberMasks) {
            valid_ = false;
            return;
        }
        out.clear();
        for (const nlohmann::json& mask : *value) {
            if (!mask.is_string()) {
                valid_ = false;
                return;
            }
            const auto& text = mask.get_ref<const std::string&>();
            if (text.empty() || text.size() > model::kMaxGrabberMaskLength ||
                text.find_first_of("\r\n\t") != std::string::npos) {
                valid_ = false;
                return;
            }
            out.push_back(text);
        }
    }

    [[nodiscard]] const nlohmann::json* Find(const char* key) const
    {
        if (!object_.is_object()) {
            return nullptr;
        }
        const auto found = object_.find(key);
        return found == object_.end() ? nullptr : &*found;
    }

private:
    const nlohmann::json& object_;
    bool& valid_;
};

template <typename Enum, typename Parse>
void EnumField(Reader& reader, const char* key, Enum& out, Parse parse, bool& valid)
{
    std::string text;
    reader.String(key, text, 64);
    if (text.empty()) {
        return;
    }
    if (const std::optional<Enum> parsed = parse(text)) {
        out = *parsed;
    } else {
        valid = false;
    }
}

nlohmann::json OptionalNumber(const std::optional<std::int64_t>& value)
{
    return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
}

std::vector<std::string> Masks(std::initializer_list<const char*> extensions)
{
    std::vector<std::string> masks;
    masks.reserve(extensions.size());
    for (const char* extension : extensions) {
        masks.push_back(std::string("*.") + extension);
    }
    return masks;
}

}  // namespace

std::string DefinitionToJson(const model::GrabberProjectDefinition& d)
{
    const nlohmann::json explore = {
        {"baseSiteDepth", d.explore.baseSiteDepth ? nlohmann::json(*d.explore.baseSiteDepth) : nlohmann::json(nullptr)},
        {"otherSitesDepth", d.explore.otherSitesDepth},
        {"noParentDirectories", d.explore.noParentDirectories},
        {"mainDomainSites", d.explore.mainDomainSites},
        {"includePaths", d.explore.includePaths},
        {"excludePaths", d.explore.excludePaths},
    };
    const nlohmann::json files = {
        {"includeTypes", d.files.includeTypes},
        {"excludeTypes", d.files.excludeTypes},
        {"minBytes", OptionalNumber(d.files.minBytes)},
        {"maxBytes", OptionalNumber(d.files.maxBytes)},
        {"includePaths", d.files.includePaths},
        {"excludePaths", d.files.excludePaths},
        {"thisSiteOnly", d.files.thisSiteOnly},
        {"hideDuplicates", d.files.hideDuplicates},
        {"downloadImmediately", d.files.downloadImmediately},
    };
    const nlohmann::json save = {
        {"mode", std::string(model::ToString(d.saveMode))},
        {"category", d.category ? nlohmann::json(d.category->value) : nlohmann::json(nullptr)},
        {"folder", d.folder},
        {"keepSiteFolders", d.keepSiteFolders},
        {"savePages", d.savePages},
        {"convertLinks", d.convertLinks},
        {"overwrite", d.overwrite},
        {"addToMainListOnClose", d.addToMainListOnClose},
    };
    const nlohmann::json document = {
        {"version", kVersion},
        {"startPage", d.startPage},
        {"template", std::string(model::ToString(d.templateKind))},
        {"save", save},
        {"explore", explore},
        {"files", files},
        {"exploreConcurrency", d.exploreConcurrency},
        {"downloadConcurrency", d.downloadConcurrency},
        {"linkTextAsDescription", d.linkTextAsDescription},
        {"useBrowserCookies", d.useBrowserCookies},
    };
    return document.dump();
}

base::Result<model::GrabberProjectDefinition> ParseDefinition(std::string_view json)
{
    const auto invalid = [] {
        return base::MakeError(base::ErrorCode::InvalidArgument, "The Grabber project is damaged and cannot be read.");
    };
    if (json.size() > kMaxDefinitionBytes) {
        return invalid();
    }
    const nlohmann::json document = nlohmann::json::parse(json, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return invalid();
    }
    const auto version = document.find("version");
    if (version == document.end() || !version->is_number_integer() || version->get<std::int64_t>() != kVersion) {
        return base::MakeError(base::ErrorCode::Unsupported,
                               "The Grabber project was made by a newer version of Last Download Manager.");
    }

    bool valid = true;
    model::GrabberProjectDefinition d;
    Reader top(document, valid);
    top.String("startPage", d.startPage, 8 * 1024);
    EnumField(top, "template", d.templateKind, model::ParseGrabberTemplate, valid);
    top.Int("exploreConcurrency", d.exploreConcurrency, 1, model::kMaxGrabberConcurrency);
    top.Int("downloadConcurrency", d.downloadConcurrency, 1, model::kMaxGrabberConcurrency);
    top.Bool("linkTextAsDescription", d.linkTextAsDescription);
    top.Bool("useBrowserCookies", d.useBrowserCookies);

    if (const nlohmann::json* saveJson = top.Find("save")) {
        Reader save(*saveJson, valid);
        EnumField(save, "mode", d.saveMode, model::ParseGrabberSaveMode, valid);
        std::optional<std::int64_t> category;
        save.OptionalInt64("category", category, 1, std::numeric_limits<std::int64_t>::max());
        if (category) {
            d.category = model::CategoryId{*category};
        }
        save.String("folder", d.folder, 4 * 1024);
        save.Bool("keepSiteFolders", d.keepSiteFolders);
        save.Bool("savePages", d.savePages);
        save.Bool("convertLinks", d.convertLinks);
        save.Bool("overwrite", d.overwrite);
        save.Bool("addToMainListOnClose", d.addToMainListOnClose);
    }
    if (const nlohmann::json* exploreJson = top.Find("explore")) {
        Reader explore(*exploreJson, valid);
        std::optional<std::int64_t> depth = d.explore.baseSiteDepth;
        explore.OptionalInt64("baseSiteDepth", depth, 0, model::kMaxGrabberDepth);
        d.explore.baseSiteDepth = depth ? std::optional<int>(static_cast<int>(*depth)) : std::nullopt;
        explore.Int("otherSitesDepth", d.explore.otherSitesDepth, 0, model::kMaxGrabberDepth);
        explore.Bool("noParentDirectories", d.explore.noParentDirectories);
        explore.Bool("mainDomainSites", d.explore.mainDomainSites);
        explore.Masks("includePaths", d.explore.includePaths);
        explore.Masks("excludePaths", d.explore.excludePaths);
    }
    if (const nlohmann::json* filesJson = top.Find("files")) {
        Reader files(*filesJson, valid);
        files.Masks("includeTypes", d.files.includeTypes);
        files.Masks("excludeTypes", d.files.excludeTypes);
        files.OptionalInt64("minBytes", d.files.minBytes, 0, std::numeric_limits<std::int64_t>::max());
        files.OptionalInt64("maxBytes", d.files.maxBytes, 0, std::numeric_limits<std::int64_t>::max());
        files.Masks("includePaths", d.files.includePaths);
        files.Masks("excludePaths", d.files.excludePaths);
        files.Bool("thisSiteOnly", d.files.thisSiteOnly);
        files.Bool("hideDuplicates", d.files.hideDuplicates);
        files.Bool("downloadImmediately", d.files.downloadImmediately);
    }

    const std::optional<core::Url> start = core::ParseUrl(d.startPage);
    if (!valid || !start || (start->scheme != "http" && start->scheme != "https")) {
        return invalid();
    }
    if (d.files.minBytes && d.files.maxBytes && *d.files.minBytes > *d.files.maxBytes) {
        return invalid();
    }
    if (d.saveMode == model::GrabberSaveMode::OneCategory && !d.category) {
        return invalid();
    }
    if (d.saveMode == model::GrabberSaveMode::OneFolder &&
        (d.folder.empty() || !std::filesystem::path(std::u8string(d.folder.begin(), d.folder.end())).is_absolute())) {
        return invalid();
    }
    return d;
}

std::vector<std::string> PictureMasks()
{
    return Masks({"jpg", "jpeg", "png", "gif", "webp", "avif", "bmp", "svg", "tif", "tiff", "ico"});
}

std::vector<std::string> AudioMasks()
{
    return Masks({"mp3", "m4a", "aac", "ogg", "oga", "opus", "flac", "wav", "wma", "mid", "midi"});
}

std::vector<std::string> VideoMasks()
{
    return Masks({"mp4", "m4v", "mkv", "webm", "avi", "mov", "wmv", "flv", "mpg", "mpeg", "3gp", "ts"});
}

model::GrabberProjectDefinition ApplyTemplate(model::GrabberProjectDefinition d, model::GrabberTemplate templateKind)
{
    d.templateKind = templateKind;
    if (templateKind == model::GrabberTemplate::Custom) {
        return d;
    }
    // Every other template starts from the same filters and changes what it is about.
    d.explore = model::GrabberExploreFilters{};
    d.files = model::GrabberFileFilters{};
    d.savePages = false;
    d.convertLinks = false;
    switch (templateKind) {
    case model::GrabberTemplate::Pictures:
        d.explore.baseSiteDepth = std::nullopt;
        d.files.includeTypes = PictureMasks();
        break;
    case model::GrabberTemplate::Audio:
        d.explore.baseSiteDepth = 1;
        d.files.includeTypes = AudioMasks();
        break;
    case model::GrabberTemplate::Video:
        d.explore.baseSiteDepth = std::nullopt;
        d.files.includeTypes = VideoMasks();
        break;
    case model::GrabberTemplate::AllFiles:
        d.explore.baseSiteDepth = 1;
        break;
    case model::GrabberTemplate::WholeSite:
        d.explore.baseSiteDepth = std::nullopt;
        d.files.thisSiteOnly = true;
        d.keepSiteFolders = true;
        d.savePages = true;
        d.convertLinks = true;
        break;
    case model::GrabberTemplate::StartPageOffline: {
        // The start page with what it shows: pictures, styles, scripts and fonts.
        d.explore.baseSiteDepth = 0;
        d.files.includeTypes = PictureMasks();
        for (const char* extension : {"css", "js", "mjs", "woff", "woff2", "ttf", "otf", "eot"}) {
            d.files.includeTypes.push_back(std::string("*.") + extension);
        }
        d.keepSiteFolders = true;
        d.savePages = true;
        d.convertLinks = true;
        break;
    }
    case model::GrabberTemplate::Custom:
        break;
    }
    return d;
}

}  // namespace ldm::grabber
