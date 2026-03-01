#pragma once

#include "../core/Download.h"
#include <mutex>
#include <wx/settings.h>
#include <wx/wx.h>

class ThemeManager {
public:
  static ThemeManager &GetInstance();

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

  mutable std::mutex m_mutex;
};
