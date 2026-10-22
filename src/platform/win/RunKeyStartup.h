#pragma once

#include "ui/StartupRegistration.h"

namespace ldm::platform {

// The per-user Run key, starting LDM minimized to the tray.
class RunKeyStartup final : public ui::StartupRegistration {
public:
    bool IsEnabled() override;
    bool SetEnabled(bool enabled) override;
};

}  // namespace ldm::platform
