#pragma once

#include "../core/Download.h"
#include <wx/settings.h>
#include <wx/wx.h>

class ThemeManager {
public:
  static ThemeManager &GetInstance();

  bool IsDarkMode() const { return m_isDarkMode; }
  void SetDarkMode(bool enable);

  // Colors
  wxColour GetBackgroundColor() const;
  wxColour GetForegroundColor() const;
  wxColour GetControlBackgroundColor() const;
  wxColour GetControlBorderColor() const;
  wxColour GetHighlightColor() const;
  wxColour GetHighlightTextColor() const;
  wxColour GetStatusColor(DownloadStatus status) const;

  // Application methods
  void ApplyTheme(wxWindow *window, bool recursive = true);
  void Initialize();

private:
  ThemeManager();
  ~ThemeManager() = default;
  ThemeManager(const ThemeManager &) = delete;
  ThemeManager &operator=(const ThemeManager &) = delete;

  bool m_isDarkMode;

  // Dark mode palette
  const wxColour m_darkBg = wxColour(32, 32, 32);
  const wxColour m_darkFg = wxColour(240, 240, 240);
  const wxColour m_darkControlBg = wxColour(45, 45, 45);
  const wxColour m_darkBorder = wxColour(60, 60, 60);
  const wxColour m_darkHighlight = wxColour(0, 120, 215);
  const wxColour m_darkHighlightText = wxColour(255, 255, 255);
};
