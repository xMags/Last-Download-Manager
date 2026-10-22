#pragma once

namespace ldm::ui {

// Launch on Windows startup for the current user (checklist V-02). Implemented in platform/win.
class StartupRegistration {
public:
    virtual ~StartupRegistration() = default;
    [[nodiscard]] virtual bool IsEnabled() = 0;
    // Returns false when Windows refused the change.
    virtual bool SetEnabled(bool enabled) = 0;
};

}  // namespace ldm::ui
