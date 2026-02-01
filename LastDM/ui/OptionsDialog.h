#pragma once

#include <wx/filepicker.h>
#include <wx/notebook.h>
#include <wx/spinctrl.h>
#include <wx/wx.h>


class OptionsDialog : public wxDialog {
public:
  OptionsDialog(wxWindow *parent);
  ~OptionsDialog() = default;

private:
  wxNotebook *m_notebook;

  // General tab controls
  wxDirPickerCtrl *m_downloadFolderPicker;
  wxCheckBox *m_autoStartCheck;
  wxCheckBox *m_minimizeToTrayCheck;
  wxCheckBox *m_showNotificationsCheck;

  // Connection tab controls
  wxSpinCtrl *m_maxConnectionsSpin;
  wxSpinCtrl *m_maxDownloadsSpin;
  wxSpinCtrl *m_speedLimitSpin;
  wxCheckBox *m_useProxyCheck;
  wxTextCtrl *m_proxyHostText;
  wxSpinCtrl *m_proxyPortSpin;

  // File types tab controls
  wxTextCtrl *m_compressedTypesText;
  wxTextCtrl *m_documentTypesText;
  wxTextCtrl *m_musicTypesText;
  wxTextCtrl *m_videoTypesText;
  wxTextCtrl *m_programTypesText;

  void CreateGeneralTab(wxNotebook *notebook);
  void CreateConnectionTab(wxNotebook *notebook);
  void CreateFileTypesTab(wxNotebook *notebook);
  void CreateInterfaceTab(wxNotebook *notebook);

  void OnOK(wxCommandEvent &event);
  void OnCancel(wxCommandEvent &event);
  void OnApply(wxCommandEvent &event);

  void LoadSettings();
  void SaveSettings();

  wxDECLARE_EVENT_TABLE();
};
