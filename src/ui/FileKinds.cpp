#include "ui/FileKinds.h"

#include "core/FileNames.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace ldm::ui {

namespace {

std::string Lower(std::string_view text)
{
    std::string lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower;
}

}  // namespace

FileKind KindOfCategory(const model::Category& category)
{
    if (!category.builtin) {
        return FileKind::Other;
    }
    static constexpr struct {
        std::string_view name;
        FileKind kind;
    } kSeeded[] = {{"Compressed", FileKind::Compressed}, {"Documents", FileKind::Documents}, {"Music", FileKind::Music},
                   {"Programs", FileKind::Programs},     {"Video", FileKind::Video}};
    for (const auto& seeded : kSeeded) {
        if (category.name == seeded.name) {
            return seeded.kind;
        }
    }
    return FileKind::Other;
}

FileKind KindOfFile(std::string_view fileName, const std::vector<model::Category>& categories)
{
    const std::string extension = Lower(core::FileExtension(std::string(fileName)));
    if (extension.empty()) {
        return FileKind::Other;
    }
    for (const model::Category& category : categories) {
        const FileKind kind = KindOfCategory(category);
        if (kind == FileKind::Other) {
            continue;
        }
        const std::string listed = Lower(category.extensions);
        size_t start = 0;
        while (start < listed.size()) {
            const size_t end = std::min(listed.find(' ', start), listed.size());
            if (listed.compare(start, end - start, extension) == 0) {
                return kind;
            }
            start = end + 1;
        }
    }
    return FileKind::Other;
}

wchar_t CategoryGlyph(FileKind kind) noexcept
{
    switch (kind) {
    case FileKind::Compressed:
        return 0xF012;  // ZipFolder
    case FileKind::Documents:
        return 0xE8A5;  // Document
    case FileKind::Music:
        return 0xEC4F;  // MusicNote
    case FileKind::Programs:
        return 0xE737;  // Favicon (a window)
    case FileKind::Video:
        return 0xE714;  // Video
    case FileKind::Other:
        break;
    }
    return 0xE8B7;  // Folder
}

wchar_t FileBadgeGlyph(FileKind kind) noexcept
{
    switch (kind) {
    case FileKind::Compressed:
        return 0xF012;  // ZipFolder
    case FileKind::Documents:
        return 0xE8A5;  // Document
    case FileKind::Music:
        return 0xEC4F;  // MusicNote
    case FileKind::Programs:
        return 0xE737;  // Favicon (a window)
    case FileKind::Video:
        return 0xF5B0;  // PlaySolid
    case FileKind::Other:
        break;
    }
    return 0;
}

}  // namespace ldm::ui
