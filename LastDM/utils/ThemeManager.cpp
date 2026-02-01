#include "ThemeManager.h"
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/combobox.h>
#include <wx/datectrl.h>
#include <wx/filepicker.h>
#include <wx/headerctrl.h>
#include <wx/listctrl.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timectrl.h>
#include <wx/toolbar.h>
#include <wx/treectrl.h>

ThemeManager &ThemeManager::GetInstance() {
  static ThemeManager instance;
  return instance;
}

ThemeManager::ThemeManager() : m_isDarkMode(false) {}

void ThemeManager::Initialize() {
  // Check system setting or load from config
  // For now default to true if the user asked for it, otherwise default to
  // false But since we are implementing it, let's default to false and let user
  // toggle
}

void ThemeManager::SetDarkMode(bool enable) { m_isDarkMode = enable; }

wxColour ThemeManager::GetBackgroundColor() const {
  return m_isDarkMode ? m_darkBg
                      : wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
}

wxColour ThemeManager::GetForegroundColor() const {
  return m_isDarkMode ? m_darkFg
                      : wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
}

wxColour ThemeManager::GetControlBackgroundColor() const {
  return m_isDarkMode ? m_darkControlBg
                      : wxSystemSettings::GetColour(wxSYS_COLOUR_LISTBOX);
}

wxColour ThemeManager::GetControlBorderColor() const {
  return m_isDarkMode ? m_darkBorder
                      : wxSystemSettings::GetColour(wxSYS_COLOUR_3DSHADOW);
}

wxColour ThemeManager::GetHighlightColor() const {
  return m_isDarkMode ? m_darkHighlight
                      : wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT);
}

wxColour ThemeManager::GetHighlightTextColor() const {
  return m_isDarkMode ? m_darkHighlightText
                      : wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHTTEXT);
}

wxColour ThemeManager::GetStatusColor(DownloadStatus status) const {
  if (m_isDarkMode) {
    switch (status) {
    case DownloadStatus::Downloading:
      return wxColour(0, 100, 0); // Dark Green
    case DownloadStatus::Paused:
      return wxColour(100, 100, 0); // Dark Yellow
    case DownloadStatus::Error:
      return wxColour(100, 0, 0); // Dark Red
    case DownloadStatus::Completed:
      return wxColour(60, 60, 60); // Dark Gray
    default:
      return m_darkBg;
    }
  } else {
    switch (status) {
    case DownloadStatus::Downloading:
      return wxColour(230, 255, 230); // Light Green
    case DownloadStatus::Paused:
      return wxColour(255, 255, 200); // Light Yellow
    case DownloadStatus::Error:
      return wxColour(255, 230, 230); // Light Red
    case DownloadStatus::Completed:
      return wxColour(240, 240, 240); // Light Gray
    default:
      return wxColour(255, 255, 255); // White
    }
  }
}

void ThemeManager::ApplyTheme(wxWindow *window, bool recursive) {
  if (!window)
    return;

  wxColour bgColor = m_isDarkMode
                         ? m_darkBg
                         : wxSystemSettings::GetColour(wxSYS_COLOUR_3DFACE);
  wxColour fgColor = m_isDarkMode
                         ? m_darkFg
                         : wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
  wxColour controlBg = m_isDarkMode
                           ? m_darkControlBg
                           : wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);

  // Apply to the window itself
  window->SetBackgroundColour(bgColor);
  window->SetForegroundColour(fgColor);

  // Recursive application for children
  if (recursive) {
    wxWindowList &children = window->GetChildren();
    for (wxWindowList::iterator iter = children.begin(); iter != children.end();
         ++iter) {
      wxWindow *child = *iter;

      // Apply colors to all common control types
      if (child->IsKindOf(CLASSINFO(wxTextCtrl)) ||
          child->IsKindOf(CLASSINFO(wxListBox)) ||
          child->IsKindOf(CLASSINFO(wxTreeCtrl)) ||
          child->IsKindOf(CLASSINFO(wxListCtrl)) ||
          child->IsKindOf(CLASSINFO(wxComboBox)) ||
          child->IsKindOf(CLASSINFO(wxChoice))) {
        // These controls use "window" style background
        child->SetBackgroundColour(controlBg);
        child->SetForegroundColour(fgColor);
      } else if (child->IsKindOf(CLASSINFO(wxStaticText))) {
        // Static text - needs foreground only, background from parent
        child->SetForegroundColour(fgColor);
        child->SetBackgroundColour(bgColor);
      } else if (child->IsKindOf(CLASSINFO(wxCheckBox))) {
        // Checkboxes need foreground color for text
        child->SetForegroundColour(fgColor);
        child->SetBackgroundColour(bgColor);
      } else if (child->IsKindOf(CLASSINFO(wxButton))) {
        // Buttons - keep system styling but set foreground
        child->SetForegroundColour(fgColor);
        if (m_isDarkMode) {
          child->SetBackgroundColour(wxColour(70, 70, 70));
        } else {
          child->SetBackgroundColour(wxNullColour);
        }
      } else if (child->IsKindOf(CLASSINFO(wxSpinCtrl))) {
        child->SetBackgroundColour(controlBg);
        child->SetForegroundColour(fgColor);
      } else if (child->IsKindOf(CLASSINFO(wxPanel))) {
        child->SetBackgroundColour(bgColor);
        child->SetForegroundColour(fgColor);
        ApplyTheme(child, true);
      } else if (child->IsKindOf(CLASSINFO(wxNotebook))) {
        child->SetBackgroundColour(bgColor);
        child->SetForegroundColour(fgColor);
        ApplyTheme(child, true);
      } else if (child->IsKindOf(CLASSINFO(wxStaticBox))) {
        child->SetForegroundColour(fgColor);
        child->SetBackgroundColour(bgColor);
        ApplyTheme(child, true);
      } else {
        // Default: apply recursively
        ApplyTheme(child, true);
      }
    }
  }

  window->Refresh();
}
