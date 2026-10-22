#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace ldm::ui {

// What happens around a finished download outside LDM itself (checklist O-09, ENG-18, P-05).
// Implemented in platform/win; tests can substitute a recorder.
class SystemActions {
public:
    virtual ~SystemActions() = default;
    virtual void PlaySoundFile(const std::filesystem::path& file) = 0;
    // Starts the virus scanner without waiting for it. Returns false when it could not start.
    virtual bool StartProgram(const std::filesystem::path& program, const std::wstring& arguments) = 0;
    virtual bool ShutDown() = 0;
    virtual bool Sleep() = 0;
    virtual bool Hibernate() = 0;
};

// Replaces every "[File]" in the scanner arguments with the quoted path (checklist ENG-18). Without
// a placeholder the quoted path is appended, so a scanner configured as just a program still
// receives the file.
[[nodiscard]] std::wstring SubstituteFileArgument(std::wstring_view arguments, const std::wstring& file);

}  // namespace ldm::ui
