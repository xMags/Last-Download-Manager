#include "ui/SmallFile.h"

#include <fstream>
#include <system_error>

namespace ldm::ui {

std::optional<std::string> ReadSmallFile(const std::filesystem::path& path, size_t limit)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > limit) {
        return std::nullopt;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::string bytes(static_cast<size_t>(size), '\0');
    file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (static_cast<size_t>(file.gcount()) != bytes.size()) {
        return std::nullopt;
    }
    return bytes;
}

}  // namespace ldm::ui
