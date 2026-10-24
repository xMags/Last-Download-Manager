#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace ldm::ui {

// What happens around a finished download outside LDM itself (checklist O-09, ENG-18, P-05), and
// what Windows allows for scheduled queues (Q-07). Implemented in platform/win; tests can
// substitute a recorder.
class SystemActions {
public:
    virtual ~SystemActions() = default;
    virtual void PlaySoundFile(const std::filesystem::path& file) = 0;
    // Starts the virus scanner without waiting for it. Returns false when it could not start.
    virtual bool StartProgram(const std::filesystem::path& program, const std::wstring& arguments) = 0;
    // With `forceTerminate`, programs that do not close by themselves are ended (a queue's option,
    // checklist Q-08); otherwise they may hold the shutdown up.
    virtual bool ShutDown(bool forceTerminate) = 0;
    virtual bool Sleep() = 0;
    virtual bool Hibernate() = 0;
    // Whether the power plan lets programs wake the computer from sleep while it is plugged in;
    // nothing when Windows does not say.
    [[nodiscard]] virtual std::optional<bool> WakeTimersAllowed() = 0;
};

// Replaces every "[File]" in the scanner arguments with the quoted path (checklist ENG-18). Without
// a placeholder the quoted path is appended, so a scanner configured as just a program still
// receives the file.
[[nodiscard]] std::wstring SubstituteFileArgument(std::wstring_view arguments, const std::wstring& file);

}  // namespace ldm::ui
