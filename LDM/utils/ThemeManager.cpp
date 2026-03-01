#include "ThemeManager.h"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/combobox.h>
#include <wx/datectrl.h>
#include <wx/filepicker.h>
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

ThemeManager::ThemeManager() {}

void ThemeManager::Initialize() {
  // Intentionally no-op for system/light-only theming.
}

wxColour ThemeManager::GetBackgroundColor() const {
  return wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
}

wxColour ThemeManager::GetForegroundColor() const {
  return wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
}

wxColour ThemeManager::GetControlBackgroundColor() const {
  return wxSystemSettings::GetColour(wxSYS_COLOUR_LISTBOX);
}

wxColour ThemeManager::GetControlBorderColor() const {
  return wxSystemSettings::GetColour(wxSYS_COLOUR_3DSHADOW);
}

wxColour ThemeManager::GetHighlightColor() const {
  return wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT);
}

wxColour ThemeManager::GetHighlightTextColor() const {
  return wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHTTEXT);
}

wxColour ThemeManager::GetStatusColor(DownloadStatus status) const {
  switch (status) {
  case DownloadStatus::Downloading:
    return wxColour(230, 255, 230);
  case DownloadStatus::Paused:
    return wxColour(255, 255, 200);
  case DownloadStatus::Error:
    return wxColour(255, 230, 230);
  case DownloadStatus::Completed:
    return wxColour(240, 240, 240);
  default:
    return wxColour(255, 255, 255);
  }
}

void ThemeManager::ApplyTheme(wxWindow *window, bool recursive) {
  if (!window) {
    return;
  }

  const wxColour bgColor = wxSystemSettings::GetColour(wxSYS_COLOUR_3DFACE);
  const wxColour fgColor = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
  const wxColour controlBg = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);

  window->SetBackgroundColour(bgColor);
  window->SetForegroundColour(fgColor);

  if (recursive) {
    wxWindowList &children = window->GetChildren();
    for (wxWindowList::iterator iter = children.begin(); iter != children.end();
         ++iter) {
      wxWindow *child = *iter;

      if (child->IsKindOf(CLASSINFO(wxTextCtrl)) ||
          child->IsKindOf(CLASSINFO(wxListBox)) ||
          child->IsKindOf(CLASSINFO(wxTreeCtrl)) ||
          child->IsKindOf(CLASSINFO(wxListCtrl)) ||
          child->IsKindOf(CLASSINFO(wxComboBox)) ||
          child->IsKindOf(CLASSINFO(wxChoice))) {
        child->SetBackgroundColour(controlBg);
        child->SetForegroundColour(fgColor);
      } else if (child->IsKindOf(CLASSINFO(wxStaticText)) ||
                 child->IsKindOf(CLASSINFO(wxCheckBox))) {
        child->SetForegroundColour(fgColor);
        child->SetBackgroundColour(bgColor);
      } else if (child->IsKindOf(CLASSINFO(wxButton))) {
        child->SetForegroundColour(fgColor);
        child->SetBackgroundColour(wxNullColour);
      } else if (child->IsKindOf(CLASSINFO(wxSpinCtrl))) {
        child->SetBackgroundColour(controlBg);
        child->SetForegroundColour(fgColor);
      } else if (child->IsKindOf(CLASSINFO(wxPanel)) ||
                 child->IsKindOf(CLASSINFO(wxNotebook)) ||
                 child->IsKindOf(CLASSINFO(wxStaticBox))) {
        child->SetBackgroundColour(bgColor);
        child->SetForegroundColour(fgColor);
        ApplyTheme(child, true);
      } else {
        ApplyTheme(child, true);
      }
    }
  }

  window->Refresh();
}
