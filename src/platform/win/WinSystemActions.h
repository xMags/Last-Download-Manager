#pragma once

#include "ui/SystemActions.h"

namespace ldm::platform {

class WinSystemActions final : public ui::SystemActions {
public:
    void PlaySoundFile(const std::filesystem::path& file) override;
    bool StartProgram(const std::filesystem::path& program, const std::wstring& arguments) override;
    bool ShutDown() override;
    bool Sleep() override;
    bool Hibernate() override;
};

}  // namespace ldm::platform
