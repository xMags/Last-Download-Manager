#include "OptionsDialog.h"
#include "../core/DownloadManager.h"
#include "../utils/Settings.h"
#include "../utils/ThemeManager.h"

enum { ID_APPLY = wxID_HIGHEST + 100 };

wxBEGIN_EVENT_TABLE(OptionsDialog, wxDialog)
    EVT_BUTTON(wxID_OK, OptionsDialog::OnOK)
        EVT_BUTTON(wxID_CANCEL, OptionsDialog::OnCancel)
            EVT_BUTTON(ID_APPLY, OptionsDialog::OnApply) wxEND_EVENT_TABLE()

                OptionsDialog::OptionsDialog(wxWindow *parent)
    : wxDialog(parent, wxID_ANY, "Options", wxDefaultPosition, wxSize(500, 450),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
  wxBoxSizer *mainSizer = new wxBoxSizer(wxVERTICAL);

  // Create notebook with tabs
  m_notebook = new wxNotebook(this, wxID_ANY);

  CreateGeneralTab(m_notebook);
  CreateConnectionTab(m_notebook);
  CreateFileTypesTab(m_notebook);
  CreateInterfaceTab(m_notebook);

  mainSizer->Add(m_notebook, 1, wxEXPAND | wxALL, 10);

  // Button sizer
  wxBoxSizer *buttonSizer = new wxBoxSizer(wxHORIZONTAL);
  buttonSizer->AddStretchSpacer();
  buttonSizer->Add(new wxButton(this, wxID_OK, "OK"), 0, wxRIGHT, 5);
  buttonSizer->Add(new wxButton(this, wxID_CANCEL, "Cancel"), 0, wxRIGHT, 5);
  buttonSizer->Add(new wxButton(this, ID_APPLY, "Apply"), 0);

  mainSizer->Add(buttonSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

  SetSizer(mainSizer);

  LoadSettings();

  // Apply current theme
  ThemeManager::GetInstance().ApplyTheme(this);

  Centre();
}

void OptionsDialog::CreateGeneralTab(wxNotebook *notebook) {
  wxPanel *panel = new wxPanel(notebook);
  wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);

  // Download folder
  wxStaticBoxSizer *folderBox =
      new wxStaticBoxSizer(wxVERTICAL, panel, "Default Download Folder");
  m_downloadFolderPicker = new wxDirPickerCtrl(
      panel, wxID_ANY, wxStandardPaths::Get().GetDocumentsDir() + "\\Downloads",
      "Select download folder");
  folderBox->Add(m_downloadFolderPicker, 0, wxEXPAND | wxALL, 5);
  sizer->Add(folderBox, 0, wxEXPAND | wxALL, 10);

  // Startup options
  wxStaticBoxSizer *startupBox =
      new wxStaticBoxSizer(wxVERTICAL, panel, "Startup");
  m_autoStartCheck =
      new wxCheckBox(panel, wxID_ANY, "Start downloads automatically");
  m_minimizeToTrayCheck =
      new wxCheckBox(panel, wxID_ANY, "Minimize to system tray on close");
  m_showNotificationsCheck =
      new wxCheckBox(panel, wxID_ANY, "Show download notifications");

  startupBox->Add(m_autoStartCheck, 0, wxALL, 5);
  startupBox->Add(m_minimizeToTrayCheck, 0, wxALL, 5);
  startupBox->Add(m_showNotificationsCheck, 0, wxALL, 5);
  sizer->Add(startupBox, 0, wxEXPAND | wxALL, 10);

  panel->SetSizer(sizer);
  notebook->AddPage(panel, "General");
}

void OptionsDialog::CreateConnectionTab(wxNotebook *notebook) {
  wxPanel *panel = new wxPanel(notebook);
  wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);

  // Connection limits
  wxStaticBoxSizer *limitsBox =
      new wxStaticBoxSizer(wxVERTICAL, panel, "Connection Limits");

  wxFlexGridSizer *gridSizer = new wxFlexGridSizer(2, 2, 5, 10);

  gridSizer->Add(new wxStaticText(panel, wxID_ANY,
                                  "Max connections per download (WinINet: 1):"),
                0, wxALIGN_CENTER_VERTICAL);
  m_maxConnectionsSpin =
      new wxSpinCtrl(panel, wxID_ANY, "1", wxDefaultPosition, wxSize(80, -1),
                     wxSP_ARROW_KEYS, 1, 1, 1);
  m_maxConnectionsSpin->Enable(false);
  gridSizer->Add(m_maxConnectionsSpin, 0);

  gridSizer->Add(
      new wxStaticText(panel, wxID_ANY, "Max simultaneous downloads:"), 0,
      wxALIGN_CENTER_VERTICAL);
  m_maxDownloadsSpin =
      new wxSpinCtrl(panel, wxID_ANY, "3", wxDefaultPosition, wxSize(80, -1),
                     wxSP_ARROW_KEYS, 1, 10, 3);
  gridSizer->Add(m_maxDownloadsSpin, 0);

  limitsBox->Add(gridSizer, 0, wxALL, 5);
  sizer->Add(limitsBox, 0, wxEXPAND | wxALL, 10);

  // Speed limit
  wxStaticBoxSizer *speedBox =
      new wxStaticBoxSizer(wxVERTICAL, panel, "Speed Limit");
  wxBoxSizer *speedSizer = new wxBoxSizer(wxHORIZONTAL);
  speedSizer->Add(new wxStaticText(
                      panel, wxID_ANY,
                      "Max download speed per download (KB/s, 0=unlimited):"),
                  0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
  m_speedLimitSpin =
      new wxSpinCtrl(panel, wxID_ANY, "0", wxDefaultPosition, wxSize(100, -1),
                     wxSP_ARROW_KEYS, 0, 100000, 0);
  speedSizer->Add(m_speedLimitSpin, 0);
  speedBox->Add(speedSizer, 0, wxALL, 5);
  sizer->Add(speedBox, 0, wxEXPAND | wxALL, 10);

  // Proxy settings
  wxStaticBoxSizer *proxyBox =
      new wxStaticBoxSizer(wxVERTICAL, panel, "Proxy Settings");
  m_useProxyCheck = new wxCheckBox(panel, wxID_ANY, "Use proxy server");
  proxyBox->Add(m_useProxyCheck, 0, wxALL, 5);

  wxFlexGridSizer *proxyGrid = new wxFlexGridSizer(2, 2, 5, 10);
  proxyGrid->Add(new wxStaticText(panel, wxID_ANY, "Proxy host:"), 0,
                 wxALIGN_CENTER_VERTICAL);
  m_proxyHostText =
      new wxTextCtrl(panel, wxID_ANY, "", wxDefaultPosition, wxSize(200, -1));
  proxyGrid->Add(m_proxyHostText, 1, wxEXPAND);
  proxyGrid->Add(new wxStaticText(panel, wxID_ANY, "Proxy port:"), 0,
                 wxALIGN_CENTER_VERTICAL);
  m_proxyPortSpin =
      new wxSpinCtrl(panel, wxID_ANY, "8080", wxDefaultPosition, wxSize(80, -1),
                     wxSP_ARROW_KEYS, 1, 65535, 8080);
  proxyGrid->Add(m_proxyPortSpin, 0);
  proxyBox->Add(proxyGrid, 0, wxALL, 5);
  sizer->Add(proxyBox, 0, wxEXPAND | wxALL, 10);

  panel->SetSizer(sizer);
  notebook->AddPage(panel, "Connection");
}

void OptionsDialog::CreateFileTypesTab(wxNotebook *notebook) {
  wxPanel *panel = new wxPanel(notebook);
  wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);

  wxStaticText *infoLabel = new wxStaticText(
      panel, wxID_ANY,
      "Define file extensions for automatic categorization (comma-separated):");
  sizer->Add(infoLabel, 0, wxALL, 10);

  wxFlexGridSizer *gridSizer = new wxFlexGridSizer(5, 2, 10, 10);
  gridSizer->AddGrowableCol(1, 1);

  gridSizer->Add(new wxStaticText(panel, wxID_ANY, "Compressed:"), 0,
                 wxALIGN_CENTER_VERTICAL);
  m_compressedTypesText = new wxTextCtrl(panel, wxID_ANY, "zip,rar,7z,tar,gz");
  gridSizer->Add(m_compressedTypesText, 1, wxEXPAND);

  gridSizer->Add(new wxStaticText(panel, wxID_ANY, "Documents:"), 0,
                 wxALIGN_CENTER_VERTICAL);
  m_documentTypesText =
      new wxTextCtrl(panel, wxID_ANY, "pdf,doc,docx,txt,xls,xlsx,ppt,pptx");
  gridSizer->Add(m_documentTypesText, 1, wxEXPAND);

  gridSizer->Add(new wxStaticText(panel, wxID_ANY, "Music:"), 0,
                 wxALIGN_CENTER_VERTICAL);
  m_musicTypesText =
      new wxTextCtrl(panel, wxID_ANY, "mp3,wav,flac,aac,ogg,wma");
  gridSizer->Add(m_musicTypesText, 1, wxEXPAND);

  gridSizer->Add(new wxStaticText(panel, wxID_ANY, "Video:"), 0,
                 wxALIGN_CENTER_VERTICAL);
  m_videoTypesText =
      new wxTextCtrl(panel, wxID_ANY, "mp4,avi,mkv,mov,wmv,flv,webm");
  gridSizer->Add(m_videoTypesText, 1, wxEXPAND);

  gridSizer->Add(new wxStaticText(panel, wxID_ANY, "Programs:"), 0,
                 wxALIGN_CENTER_VERTICAL);
  m_programTypesText =
      new wxTextCtrl(panel, wxID_ANY, "exe,msi,dmg,deb,rpm,apk");
  gridSizer->Add(m_programTypesText, 1, wxEXPAND);

  sizer->Add(gridSizer, 0, wxEXPAND | wxALL, 10);

  panel->SetSizer(sizer);
  notebook->AddPage(panel, "File Types");
}

void OptionsDialog::CreateInterfaceTab(wxNotebook *notebook) {
  wxPanel *panel = new wxPanel(notebook);
  wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);

  wxStaticBoxSizer *appearanceBox =
      new wxStaticBoxSizer(wxVERTICAL, panel, "Appearance");

  // Theme selection (placeholder)
  wxBoxSizer *themeSizer = new wxBoxSizer(wxHORIZONTAL);
  themeSizer->Add(new wxStaticText(panel, wxID_ANY, "Theme:"), 0,
                  wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
  wxArrayString themes;
  themes.Add("Light");
  themes.Add("Dark");
  themes.Add("System Default");
  wxChoice *themeChoice =
      new wxChoice(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, themes);
  themeChoice->SetSelection(0);
  themeSizer->Add(themeChoice, 0);
  appearanceBox->Add(themeSizer, 0, wxALL, 5);

  // Show toolbar text
  wxCheckBox *showToolbarTextCheck =
      new wxCheckBox(panel, wxID_ANY, "Show text on toolbar buttons");
  showToolbarTextCheck->SetValue(true);
  appearanceBox->Add(showToolbarTextCheck, 0, wxALL, 5);

  // Show status bar
  wxCheckBox *showStatusBarCheck =
      new wxCheckBox(panel, wxID_ANY, "Show status bar");
  showStatusBarCheck->SetValue(true);
  appearanceBox->Add(showStatusBarCheck, 0, wxALL, 5);

  sizer->Add(appearanceBox, 0, wxEXPAND | wxALL, 10);

  panel->SetSizer(sizer);
  notebook->AddPage(panel, "Interface");
}

void OptionsDialog::LoadSettings() {
  Settings &settings = Settings::GetInstance();

  m_downloadFolderPicker->SetPath(settings.GetDownloadFolder());
  m_autoStartCheck->SetValue(settings.GetAutoStart());
  m_minimizeToTrayCheck->SetValue(settings.GetMinimizeToTray());
  m_showNotificationsCheck->SetValue(settings.GetShowNotifications());
  m_maxConnectionsSpin->SetValue(settings.GetMaxConnections());
  m_maxDownloadsSpin->SetValue(settings.GetMaxSimultaneousDownloads());
  m_speedLimitSpin->SetValue(settings.GetSpeedLimit());
  m_useProxyCheck->SetValue(settings.GetUseProxy());
  m_proxyHostText->SetValue(settings.GetProxyHost());
  m_proxyPortSpin->SetValue(settings.GetProxyPort());
}

void OptionsDialog::SaveSettings() {
  Settings &settings = Settings::GetInstance();

  settings.SetDownloadFolder(m_downloadFolderPicker->GetPath());
  settings.SetAutoStart(m_autoStartCheck->GetValue());
  settings.SetMinimizeToTray(m_minimizeToTrayCheck->GetValue());
  settings.SetShowNotifications(m_showNotificationsCheck->GetValue());
  settings.SetMaxConnections(m_maxConnectionsSpin->GetValue());
  settings.SetMaxSimultaneousDownloads(m_maxDownloadsSpin->GetValue());
  settings.SetSpeedLimit(m_speedLimitSpin->GetValue());
  settings.SetUseProxy(m_useProxyCheck->GetValue());
  settings.SetProxyHost(m_proxyHostText->GetValue().ToStdString());
  settings.SetProxyPort(m_proxyPortSpin->GetValue());

  settings.Save();

  DownloadManager::GetInstance().ApplySettings(settings);
}

void OptionsDialog::OnOK(wxCommandEvent &event) {
  SaveSettings();
  EndModal(wxID_OK);
}

void OptionsDialog::OnCancel(wxCommandEvent &event) { EndModal(wxID_CANCEL); }

void OptionsDialog::OnApply(wxCommandEvent &event) { SaveSettings(); }
