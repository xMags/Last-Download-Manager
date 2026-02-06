#pragma once

#include "../core/YtDlpManager.h"
#include <wx/wx.h>
#include <wx/listctrl.h>
#include <vector>
#include <string>

class VideoQualityDialog : public wxDialog {
public:
  VideoQualityDialog(wxWindow *parent, const std::string &url, const std::string &title);
  ~VideoQualityDialog() = default;

  // Get selected format ID (empty if cancelled)
  std::string GetSelectedFormatId() const { return m_selectedFormat; }

  // Check if user wants to remember this choice
  bool RememberChoice() const { return m_rememberChoice; }

private:
  void OnOK(wxCommandEvent &event);
  void OnCancel(wxCommandEvent &event);
  void OnFormatSelected(wxListEvent &event);
  void OnFormatActivated(wxListEvent &event);
  void PopulateFormats();

  std::string m_url;
  std::string m_videoTitle;
  std::string m_selectedFormat;
  bool m_rememberChoice;

  wxListCtrl *m_formatList;
  wxCheckBox *m_rememberCheck;
  wxStaticText *m_titleLabel;
  wxStaticText *m_statusLabel;

  std::vector<VideoFormat> m_formats;

  wxDECLARE_EVENT_TABLE();
};
