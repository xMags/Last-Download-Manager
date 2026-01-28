#include "MainWindow.h"
#include "../core/DownloadManager.h"
#include "../utils/ThemeManager.h"
#include "OptionsDialog.h"
#include "SchedulerDialog.h"
#include <wx/filename.h>
#include <wx/msgdlg.h>
#include <wx/stdpaths.h>

wxBEGIN_EVENT_TABLE(MainWindow, wxFrame) EVT_MENU(
    wxID_EXIT, MainWindow::OnExit) EVT_MENU(wxID_ABOUT, MainWindow::OnAbout)
    EVT_MENU(ID_ADD_URL, MainWindow::OnAddUrl) EVT_MENU(
        ID_RESUME, MainWindow::OnResume) EVT_MENU(ID_PAUSE, MainWindow::OnPause)
        EVT_MENU(ID_DELETE, MainWindow::OnDelete) EVT_MENU(
            ID_OPTIONS, MainWindow::OnOptions) EVT_TOOL(ID_ADD_URL,
                                                        MainWindow::OnAddUrl)
            EVT_TOOL(ID_RESUME, MainWindow::OnResume) EVT_TOOL(
                ID_PAUSE, MainWindow::OnPause) EVT_TOOL(ID_DELETE,
                                                        MainWindow::OnDelete)
                EVT_TOOL(ID_OPTIONS, MainWindow::OnOptions) EVT_TOOL(
                    ID_SCHEDULER,
                    MainWindow::OnScheduler) EVT_TOOL(ID_START_QUEUE,
                                                      MainWindow::OnStartQueue)
                    EVT_TOOL(ID_STOP_QUEUE, MainWindow::OnStopQueue) EVT_MENU(
                        ID_SCHEDULER, MainWindow::OnScheduler)
                        EVT_MENU(ID_START_QUEUE, MainWindow::OnStartQueue)
                            EVT_MENU(ID_STOP_QUEUE, MainWindow::OnStopQueue)
                                EVT_TIMER(ID_UPDATE_TIMER,
                                          MainWindow::OnUpdateTimer)
                                    EVT_TREE_SEL_CHANGED(
                                        wxID_ANY,
                                        MainWindow::OnCategorySelected)
                                        EVT_MENU(ID_VIEW_DARK_MODE,
                                                 MainWindow::OnViewDarkMode)
                                            EVT_ICONIZE(MainWindow::OnIconize)
                                                EVT_CLOSE(MainWindow::OnClose)
                                                    wxEND_EVENT_TABLE()

    // Event table for system tray icon
    wxBEGIN_EVENT_TABLE(LastDMTaskBarIcon, wxTaskBarIcon)
        EVT_MENU(ID_TRAY_SHOW, LastDMTaskBarIcon::OnTrayShow)
            EVT_MENU(ID_TRAY_EXIT, LastDMTaskBarIcon::OnTrayExit)
                EVT_TASKBAR_LEFT_DCLICK(LastDMTaskBarIcon::OnLeftClick)
                    wxEND_EVENT_TABLE()

                        MainWindow::MainWindow()
    : wxFrame(nullptr, wxID_ANY, "Last Download Manager", wxDefaultPosition,
              wxSize(1050, 700)),
      m_splitter(nullptr), m_categoriesPanel(nullptr),
      m_downloadsTable(nullptr), m_toolbar(nullptr), m_statusBar(nullptr),
      m_updateTimer(nullptr), m_taskBarIcon(nullptr) {
  // Set window icon from PNG file
  // Set window icon from PNG file
  wxString exePath = wxStandardPaths::Get().GetExecutablePath();

  // Try finding icon in ./resources/ (release layout)
  wxFileName iconPath(exePath);
  iconPath.SetFullName("icon_32.png");
  iconPath.AppendDir("resources");
  iconPath.Normalize();

  if (!wxFileExists(iconPath.GetFullPath())) {
    // Fallback: Try ../resources/ (dev layout)
    iconPath = wxFileName(exePath);
    iconPath.SetFullName("icon_32.png");
    iconPath.AppendDir("..");
    iconPath.AppendDir("resources");
    iconPath.Normalize();
  }

  if (wxFileExists(iconPath.GetFullPath())) {
    wxIcon appIcon;
    appIcon.LoadFile(iconPath.GetFullPath(), wxBITMAP_TYPE_PNG);
    if (appIcon.IsOk()) {
      SetIcon(appIcon);
    }
  }

  // Set minimum size
  SetMinSize(wxSize(640, 480));

  // Initialize all UI components
  CreateMenuBar();
  CreateToolBar();
  CreateStatusBar();
  CreateMainContent();

  // Initialize theme
  ThemeManager::GetInstance().Initialize();
  ThemeManager::GetInstance().ApplyTheme(this);

  // Load existing downloads from database into the table
  DownloadManager &manager = DownloadManager::GetInstance();
  auto downloads = manager.GetAllDownloads();
  for (const auto &download : downloads) {
    m_downloadsTable->AddDownload(download);
  }

  // Start update timer (500ms interval for UI refresh)
  m_updateTimer = new wxTimer(this, ID_UPDATE_TIMER);
  m_updateTimer->Start(500);

  // Center on screen
  Centre();
}

MainWindow::~MainWindow() {
  if (m_updateTimer) {
    m_updateTimer->Stop();
    delete m_updateTimer;
  }
}

void MainWindow::CreateMenuBar() {
  m_menuBar = new wxMenuBar();

  // Tasks menu
  m_tasksMenu = new wxMenu();
  m_tasksMenu->Append(ID_ADD_URL, "Add &URL...\tCtrl+N",
                      "Add a new download URL");
  m_tasksMenu->AppendSeparator();
  m_tasksMenu->Append(ID_RESUME, "&Resume\tCtrl+R", "Resume selected download");
  m_tasksMenu->Append(ID_PAUSE, "&Pause\tCtrl+P", "Pause selected download");
  m_tasksMenu->Append(ID_STOP, "&Stop", "Stop selected download");
  m_tasksMenu->Append(ID_STOP_ALL, "Stop &All", "Stop all downloads");
  m_tasksMenu->AppendSeparator();
  m_tasksMenu->Append(wxID_EXIT, "E&xit\tAlt+F4", "Exit the application");
  m_menuBar->Append(m_tasksMenu, "&Tasks");

  // File menu
  m_fileMenu = new wxMenu();
  m_fileMenu->Append(ID_DELETE, "&Delete\tDel", "Delete selected download");
  m_fileMenu->Append(ID_DELETE_COMPLETED, "Delete &Completed",
                     "Delete completed downloads");
  m_menuBar->Append(m_fileMenu, "&File");

  // Downloads menu
  m_downloadsMenu = new wxMenu();
  m_downloadsMenu->Append(ID_SCHEDULER, "&Scheduler...", "Open scheduler");
  m_downloadsMenu->Append(ID_START_QUEUE, "Start &Queue",
                          "Start download queue");
  m_downloadsMenu->Append(ID_STOP_QUEUE, "Stop Q&ueue", "Stop download queue");
  m_downloadsMenu->AppendSeparator();
  m_downloadsMenu->Append(ID_GRABBER, "&Grabber...", "Open URL grabber");
  m_menuBar->Append(m_downloadsMenu, "&Downloads");

  // View menu
  m_viewMenu = new wxMenu();
  m_viewMenu->AppendCheckItem(ID_CATEGORIES_PANEL, "&Categories Panel",
                              "Show/hide categories panel");
  m_viewMenu->Check(ID_CATEGORIES_PANEL, true);

  // Dark mode temporarily disabled
  // m_viewMenu->AppendCheckItem(ID_VIEW_DARK_MODE, "&Dark Mode",
  //                             "Toggle dark/light theme");
  // m_viewMenu->Check(ID_VIEW_DARK_MODE,
  //                   ThemeManager::GetInstance().IsDarkMode());

  m_viewMenu->AppendSeparator();
  m_viewMenu->Append(ID_OPTIONS, "&Options...\tCtrl+O", "Open options dialog");
  m_menuBar->Append(m_viewMenu, "&View");

  // Help menu
  m_helpMenu = new wxMenu();
  m_helpMenu->Append(wxID_ABOUT, "&About...", "About Last Download Manager");
  m_menuBar->Append(m_helpMenu, "&Help");

  SetMenuBar(m_menuBar);
}

void MainWindow::CreateToolBar() {
  wxFrame::CreateToolBar(wxTB_HORIZONTAL | wxTB_TEXT);
  m_toolbar = GetToolBar();
  m_toolbar->SetToolBitmapSize(wxSize(32, 32));

  // Helper lambda to load toolbar icon
  auto loadIcon = [](const wxString &name) -> wxBitmap {
    // Try embedded resource first (Permanent Cure)
    wxIcon resIcon(name, wxBITMAP_TYPE_ICO_RESOURCE);
    if (resIcon.IsOk()) {
      wxBitmap bmp(resIcon);
      wxImage img = bmp.ConvertToImage();
      img.Rescale(32, 32, wxIMAGE_QUALITY_HIGH);
      return wxBitmap(img);
    }

    // Fallback: Get executable path and construct icon path relative to it
    wxString exePath = wxStandardPaths::Get().GetExecutablePath();

    // Try ./resources first
    wxFileName iconPath(exePath);
    iconPath.SetFullName(name + ".png");
    iconPath.AppendDir("resources");
    iconPath.Normalize();

    if (!wxFileExists(iconPath.GetFullPath())) {
      // Try ../resources
      iconPath = wxFileName(exePath);
      iconPath.SetFullName(name + ".png");
      iconPath.AppendDir("..");
      iconPath.AppendDir("resources");
      iconPath.Normalize();
    }

    wxString path = iconPath.GetFullPath();
    wxImage img(path, wxBITMAP_TYPE_PNG);
    if (img.IsOk()) {
      img.Rescale(32, 32, wxIMAGE_QUALITY_HIGH);
      return wxBitmap(img);
    }
    // Fallback to default art if icon not found
    return wxArtProvider::GetBitmap(wxART_NORMAL_FILE, wxART_TOOLBAR,
                                    wxSize(32, 32));
  };

  // Add toolbar buttons with custom icons
  m_toolbar->AddTool(ID_ADD_URL, "Add URL", loadIcon("icon_add_url"),
                     "Add a new download URL");

  m_toolbar->AddTool(ID_RESUME, "Resume", loadIcon("icon_resume"),
                     "Resume download");

  m_toolbar->AddTool(ID_PAUSE, "Pause", loadIcon("icon_pause"),
                     "Pause download");

  m_toolbar->AddSeparator();

  m_toolbar->AddTool(ID_DELETE, "Delete", loadIcon("icon_delete"),
                     "Delete download");

  m_toolbar->AddSeparator();

  m_toolbar->AddTool(ID_OPTIONS, "Options", loadIcon("icon_options"),
                     "Open options");

  m_toolbar->AddTool(ID_SCHEDULER, "Scheduler", loadIcon("icon_scheduler"),
                     "Open scheduler");

  m_toolbar->AddSeparator();

  m_toolbar->AddTool(ID_START_QUEUE, "Start Queue",
                     loadIcon("icon_start_queue"), "Start download queue");

  m_toolbar->AddTool(ID_STOP_QUEUE, "Stop Queue", loadIcon("icon_stop_queue"),
                     "Stop download queue");

  m_toolbar->AddSeparator();

  m_toolbar->AddTool(ID_GRABBER, "Grabber", loadIcon("icon_grabber"),
                     "Open URL grabber");

  m_toolbar->Realize();
}

void MainWindow::CreateStatusBar() {
  m_statusBar = wxFrame::CreateStatusBar(3);
  int widths[] = {-3, -1, -1};
  m_statusBar->SetStatusWidths(3, widths);
  m_statusBar->SetStatusText("Ready", 0);
  m_statusBar->SetStatusText("Downloads: 0", 1);
  m_statusBar->SetStatusText("Speed: 0 KB/s", 2);
}

void MainWindow::CreateMainContent() {
  // Create main vertical sizer for splitter + speed graph
  wxBoxSizer *mainSizer = new wxBoxSizer(wxVERTICAL);

  // Create splitter window for resizable panels
  m_splitter =
      new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                           wxSP_BORDER | wxSP_LIVE_UPDATE);

  // Create categories panel (left side)
  m_categoriesPanel = new CategoriesPanel(m_splitter);

  // Create downloads table (right side)
  m_downloadsTable = new DownloadsTable(m_splitter);

  // Split the window
  m_splitter->SplitVertically(m_categoriesPanel, m_downloadsTable, 180);
  m_splitter->SetMinimumPaneSize(100);

  // Add splitter to main sizer (takes most space)
  mainSizer->Add(m_splitter, 1, wxEXPAND);

  // Create speed graph at the bottom
  m_speedGraph = new SpeedGraphPanel(this);
  mainSizer->Add(m_speedGraph, 0, wxEXPAND | wxALL, 2);

  SetSizer(mainSizer);
}

void MainWindow::OnExit(wxCommandEvent &event) { Close(true); }

void MainWindow::OnAbout(wxCommandEvent &event) {
  wxMessageBox("Last Download Manager\n\n"
               "Version 1.0\n\n"
               "A powerful download manager built with wxWidgets, libcurl, and "
               "SQLite.\n\n"
               "Features:\n"
               "- Multi-threaded downloads\n"
               "- Pause/Resume support\n"
               "- Automatic file categorization\n"
               "- Download scheduling",
               "About Last Download Manager", wxOK | wxICON_INFORMATION, this);
}

void MainWindow::OnAddUrl(wxCommandEvent &event) {
  wxTextEntryDialog dialog(this,
                           "Enter the URL to download:", "Add New Download", "",
                           wxOK | wxCANCEL | wxCENTRE);

  if (dialog.ShowModal() == wxID_OK) {
    wxString url = dialog.GetValue();
    if (!url.IsEmpty()) {
      // Add download to the manager
      DownloadManager &manager = DownloadManager::GetInstance();
      int downloadId = manager.AddDownload(url.ToStdString());

      // Check for validation error
      if (downloadId < 0) {
        wxMessageBox(
            "Invalid URL. Please enter a valid HTTP, HTTPS, or FTP URL.",
            "Invalid URL", wxOK | wxICON_ERROR, this);
        m_statusBar->SetStatusText("Invalid URL entered", 0);
        return;
      }

      // Get the download object and add to table
      auto download = manager.GetDownload(downloadId);
      if (download) {
        m_downloadsTable->AddDownload(download);

        // Start the download automatically
        manager.StartDownload(downloadId);

        // Update status bar
        m_statusBar->SetStatusText("Downloading: " + url, 0);
        m_statusBar->SetStatusText(
            wxString::Format("Downloads: %d", manager.GetTotalDownloads()), 1);
      }
    }
  }
}

void MainWindow::OnResume(wxCommandEvent &event) {
  int selectedId = m_downloadsTable->GetSelectedDownloadId();
  if (selectedId >= 0) {
    DownloadManager::GetInstance().ResumeDownload(selectedId);
    m_statusBar->SetStatusText("Resuming download...", 0);
  } else {
    m_statusBar->SetStatusText("No download selected", 0);
  }
}

void MainWindow::OnPause(wxCommandEvent &event) {
  int selectedId = m_downloadsTable->GetSelectedDownloadId();
  if (selectedId >= 0) {
    DownloadManager::GetInstance().PauseDownload(selectedId);
    m_statusBar->SetStatusText("Download paused", 0);
  } else {
    m_statusBar->SetStatusText("No download selected", 0);
  }
}

void MainWindow::OnDelete(wxCommandEvent &event) {
  int selectedId = m_downloadsTable->GetSelectedDownloadId();
  if (selectedId < 0) {
    m_statusBar->SetStatusText("No download selected", 0);
    return;
  }

  int result =
      wxMessageBox("Are you sure you want to delete the selected download?",
                   "Confirm Delete", wxYES_NO | wxICON_QUESTION, this);

  if (result == wxYES) {
    DownloadManager::GetInstance().RemoveDownload(selectedId);
    m_downloadsTable->RemoveDownload(selectedId);
    m_statusBar->SetStatusText("Download deleted", 0);
    m_statusBar->SetStatusText(
        wxString::Format("Downloads: %d",
                         DownloadManager::GetInstance().GetTotalDownloads()),
        1);
  }
}

void MainWindow::OnOptions(wxCommandEvent &event) {
  OptionsDialog dialog(this);
  dialog.ShowModal();
}

void MainWindow::OnScheduler(wxCommandEvent &event) {
  SchedulerDialog dialog(this);
  if (dialog.ShowModal() == wxID_OK) {
    DownloadManager &manager = DownloadManager::GetInstance();
    manager.SetSchedule(
        dialog.IsStartTimeEnabled(), dialog.GetStartTime(),
        dialog.IsStopTimeEnabled(), dialog.GetStopTime(),
        dialog.GetMaxConcurrentDownloads(), dialog.ShouldHangUpWhenDone(),
        dialog.ShouldExitWhenDone(), dialog.ShouldShutdownWhenDone());
  }
}

void MainWindow::OnStartQueue(wxCommandEvent &event) {
  DownloadManager::GetInstance().StartQueue();
  m_statusBar->SetStatusText("Download queue started", 0);
}

void MainWindow::OnStopQueue(wxCommandEvent &event) {
  DownloadManager::GetInstance().StopQueue();
  m_statusBar->SetStatusText("Download queue stopped", 0);
}

void MainWindow::OnViewDarkMode(wxCommandEvent &event) {
  bool isDarkMode = event.IsChecked();
  ThemeManager::GetInstance().SetDarkMode(isDarkMode);
  ThemeManager::GetInstance().ApplyTheme(this);

  // Refresh specific controls that might need re-layout or data refresh
  if (m_downloadsTable) {
    m_downloadsTable->RefreshAll();
  }
}

void MainWindow::OnCategorySelected(wxTreeEvent &event) {
  // Guard against this being called during construction before m_downloadsTable
  // exists
  if (!m_downloadsTable) {
    return;
  }

  // Get the selected category from the categories panel
  wxString category = m_categoriesPanel->GetSelectedCategory();

  // Filter downloads table by this category
  m_downloadsTable->FilterByCategory(category);
}

void MainWindow::OnUpdateTimer(wxTimerEvent &event) {
  // Refresh downloads table with latest data from DownloadManager
  DownloadManager &manager = DownloadManager::GetInstance();
  auto downloads = manager.GetAllDownloads();

  // Update each download in the table
  for (const auto &download : downloads) {
    m_downloadsTable->UpdateDownload(download->GetId());
  }

  // Update status bar
  int active = manager.GetActiveDownloads();
  double speed = manager.GetTotalSpeed();

  if (active > 0) {
    m_statusBar->SetStatusText(wxString::Format("Downloading: %d", active), 0);
  }

  m_statusBar->SetStatusText(
      wxString::Format("Downloads: %d", manager.GetTotalDownloads()), 1);

  // Format speed
  wxString speedStr;
  if (speed >= 1024 * 1024) {
    speedStr = wxString::Format("Speed: %.1f MB/s", speed / (1024 * 1024));
  } else if (speed >= 1024) {
    speedStr = wxString::Format("Speed: %.1f KB/s", speed / 1024);
  } else {
    speedStr = wxString::Format("Speed: %.0f B/s", speed);
  }
  m_statusBar->SetStatusText(speedStr, 2);

  // Update speed graph with current total speed
  if (m_speedGraph) {
    m_speedGraph->UpdateSpeed(speed);
  }
}

// System tray handlers
void MainWindow::OnIconize(wxIconizeEvent &event) {
  if (event.IsIconized()) {
    // Minimize to system tray
    m_minimizedToTray = true;
    Hide();

    // Create tray icon if not already created
    if (!m_taskBarIcon) {
      m_taskBarIcon = new LastDMTaskBarIcon(this);
      wxIcon trayIcon;
      wxString exePath = wxStandardPaths::Get().GetExecutablePath();

      // Try ./resources first
      wxFileName iconPath(exePath);
      iconPath.SetFullName("icon_32.png");
      iconPath.AppendDir("resources");
      iconPath.Normalize();

      if (!wxFileExists(iconPath.GetFullPath())) {
        // Try ../resources
        iconPath = wxFileName(exePath);
        iconPath.SetFullName("icon_32.png");
        iconPath.AppendDir("..");
        iconPath.AppendDir("resources");
        iconPath.Normalize();
      }
      if (wxFileExists(iconPath.GetFullPath())) {
        trayIcon.LoadFile(iconPath.GetFullPath(), wxBITMAP_TYPE_PNG);
      }
      if (!trayIcon.IsOk()) {
        trayIcon = wxArtProvider::GetIcon(wxART_EXECUTABLE_FILE);
      }
      m_taskBarIcon->SetIcon(trayIcon, "Last Download Manager");
    }
  }
  event.Skip();
}

void MainWindow::OnClose(wxCloseEvent &event) {
  // Clean up tray icon before closing
  if (m_taskBarIcon) {
    m_taskBarIcon->RemoveIcon();
    delete m_taskBarIcon;
    m_taskBarIcon = nullptr;
  }
  event.Skip();
}

void MainWindow::ShowFromTray() {
  Show(true);
  Iconize(false);
  Raise();
  m_minimizedToTray = false;
}

void MainWindow::ShowNotification(const wxString &title,
                                  const wxString &message) {
  if (m_taskBarIcon && m_minimizedToTray) {
    m_taskBarIcon->ShowBalloon(title, message, 3000, wxICON_INFORMATION);
  }
}

// LastDMTaskBarIcon implementations
void LastDMTaskBarIcon::OnTrayShow(wxCommandEvent &event) {
  m_parent->ShowFromTray();
}

void LastDMTaskBarIcon::OnTrayExit(wxCommandEvent &event) {
  m_parent->Close(true);
}

void LastDMTaskBarIcon::OnLeftClick(wxTaskBarIconEvent &event) {
  m_parent->ShowFromTray();
}
