#pragma once

#include "ui/Theme.h"

#include <windows.h>

#include <string>

namespace ldm::ui {

enum class MessageKind { Information, Warning, Question };
enum class MessageButtons { Ok, YesNo };

struct MessageRequest {
    std::wstring text;
    MessageKind kind = MessageKind::Information;
    MessageButtons buttons = MessageButtons::Ok;
    bool bringToFront = false;  // for questions that can arrive while LDM is in the background
};

// Tells or asks the user something, like MessageBox. The Windows message box has no dark mode
// (checklist V-08), so while `theme` is dark this shows LDM's own dialog with the same icon,
// sound and buttons; while it is light it is the Windows message box. Yes/No questions cannot
// be dismissed with Esc or the close button, as in the Windows message box.
// Returns IDOK, IDYES or IDNO.
int ShowMessage(HWND owner, const Theme& theme, const MessageRequest& request);

}  // namespace ldm::ui
