#include "MainWindow.h"
#include "../core/DownloadManager.h"
#include "../core/YtDlpManager.h"
#include "../database/DatabaseManager.h"
#include "../utils/HttpServer.h"
#include "../utils/ThemeManager.h"
#include "OptionsDialog.h"
#include "SchedulerDialog.h"
#include "VideoQualityDialog.h"
#include <wx/dnd.h>
#include <wx/file.h>
#include <wx/dir.h>
#include <wx/utils.h>
#include <wx/filename.h>
#include <wx/msgdlg.h>
#include <wx/stdpaths.h>
#include <wx/progdlg.h>
#include <vector>
#include <thread>
#include <iostream>

#ifdef _WIN32
#include <Windows.h>
#endif



// Drop target for URLs
class URLDropTarget : public wxTextDropTarget {
public:
  URLDropTarget(MainWindow *parent) : m_parent(parent) {}

  bool OnDropText(wxCoord x, wxCoord y, const wxString &text) override {
    if (m_parent) {
      m_parent->ProcessUrl(text);
      return true;
    }
    return false;
  }

private:
  MainWindow *m_parent;
};

wxBEGIN_EVENT_TABLE(MainWindow, wxFrame) EVT_MENU(
    wxID_EXIT, MainWindow::OnExit) EVT_MENU(wxID_ABOUT, MainWindow::OnAbout)
    EVT_MENU(ID_ADD_URL, MainWindow::OnAddUrl) EVT_MENU(
        ID_RESUME, MainWindow::OnResume) EVT_MENU(ID_PAUSE, MainWindow::OnPause)
        EVT_MENU(ID_STOP, MainWindow::OnStop) EVT_MENU(
            ID_STOP_ALL, MainWindow::OnStopAll) EVT_MENU(
            ID_DELETE, MainWindow::OnDelete) EVT_MENU(
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
                                                                                        EVT_MENU(ID_INSTALL_EXTENSION, MainWindow::OnInstallExtension)
                                                                                        EVT_MENU(ID_GRABBER, MainWindow::OnGrabber)
                                                                                        EVT_TOOL(ID_GRABBER, MainWindow::OnGrabber)
                                                                                        wxEND_EVENT_TABLE()

                                                            MainWindow::MainWindow()
                                        : wxFrame(nullptr, wxID_ANY, "Last Download Manager v2.0.0", wxDefaultPosition,
                                                  wxSize(1050, 700)),
                                          m_splitter(nullptr), m_categoriesPanel(nullptr),
                                          m_downloadsTable(nullptr), m_toolbar(nullptr), m_statusBar(nullptr),
                                          m_updateTimer(nullptr), m_speedGraph(nullptr), m_taskBarIcon(nullptr) {
  // Set drop target
  SetDropTarget(new URLDropTarget(this));

  // Set window icon
  wxIcon appIcon("IDI_ICON1", wxBITMAP_TYPE_ICO_RESOURCE);
  if (appIcon.IsOk()) {
    SetIcon(appIcon);
  } else {
    // Fallback to loading from file
    wxString exePath = wxStandardPaths::Get().GetExecutablePath();
    wxFileName iconPath(exePath);
    iconPath.SetFullName("app_icon.ico");
    iconPath.AppendDir("resources");
    iconPath.Normalize(wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE | wxPATH_NORM_TILDE);

    if (!wxFileExists(iconPath.GetFullPath())) {
      iconPath = wxFileName(exePath);
      iconPath.SetFullName("app_icon.ico");
      iconPath.AppendDir("..");
      iconPath.AppendDir("resources");
      iconPath.Normalize(wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE | wxPATH_NORM_TILDE);
    }

    if (wxFileExists(iconPath.GetFullPath())) {
      appIcon.LoadFile(iconPath.GetFullPath(), wxBITMAP_TYPE_ICO);
      if (appIcon.IsOk()) {
        SetIcon(appIcon);
      }
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

  // Start HTTP server for browser extension integration
  HttpServer &httpServer = HttpServer::GetInstance();
  httpServer.SetUrlCallback([this](const std::string &url, const std::string &referer) {
    // Make explicit copy to ensure it survives until CallAfter executes
    std::string urlCopy = url;
    std::string refererCopy = referer;
    std::cout << "[MainWindow] Callback received URL (len=" << urlCopy.length() << "): " << urlCopy << std::endl;
    if (!refererCopy.empty()) {
      std::cout << "[MainWindow] With referer: " << refererCopy << std::endl;
    }
    std::cout.flush();
    // Post to main thread via wxWidgets event mechanism
    wxTheApp->CallAfter([this, urlCopy, refererCopy]() {
      std::cout << "[MainWindow] CallAfter executing with URL (len=" << urlCopy.length() << ")" << std::endl;
      std::cout.flush();
      ProcessUrl(wxString::FromUTF8(urlCopy), wxString::FromUTF8(refererCopy));
    });
  });

  // Set status callback for extension to get download speeds
  httpServer.SetStatusCallback([]() -> std::string {
    DownloadManager &mgr = DownloadManager::GetInstance();
    auto downloads = mgr.GetAllDownloads();

    int activeCount = 0;
    double totalSpeed = 0;
    std::string downloadsJson = "[";
    bool first = true;

    for (const auto &dl : downloads) {
      DownloadStatus status = dl->GetStatus();
      if (status == DownloadStatus::Downloading) {
        activeCount++;
        double speed = dl->GetSpeed();
        totalSpeed += speed;

        if (!first) downloadsJson += ",";
        first = false;

        // Escape filename for JSON
        std::string filename = dl->GetFilename();
        std::string escapedFilename;
        escapedFilename.reserve(filename.size() + 16);
        for (char c : filename) {
          if (c == '"') escapedFilename += "\\\"";
          else if (c == '\\') escapedFilename += "\\\\";
          else if (c == '\n') escapedFilename += "\\n";
          else if (c == '\r') escapedFilename += "\\r";
          else if (c == '\t') escapedFilename += "\\t";
          else escapedFilename += c;
        }

        // Build JSON using string concatenation (no buffer overflow risk)
        downloadsJson += "{\"id\":";
        downloadsJson += std::to_string(dl->GetId());
        downloadsJson += ",\"filename\":\"";
        downloadsJson += escapedFilename;
        downloadsJson += "\",\"progress\":";
        downloadsJson += std::to_string(static_cast<int>(dl->GetProgress() * 10) / 10.0).substr(0, 5);
        downloadsJson += ",\"speed\":";
        downloadsJson += std::to_string(static_cast<long long>(speed));
        downloadsJson += ",\"size\":";
        downloadsJson += std::to_string(dl->GetTotalSize());
        downloadsJson += ",\"downloaded\":";
        downloadsJson += std::to_string(dl->GetDownloadedSize());
        downloadsJson += "}";
      }
    }
    downloadsJson += "]";

    // Build final result using string concatenation
    std::string result = "{\"status\":\"ok\",\"activeDownloads\":";
    result += std::to_string(activeCount);
    result += ",\"totalSpeed\":";
    result += std::to_string(static_cast<long long>(totalSpeed));
    result += ",\"downloads\":";
    result += downloadsJson;
    result += "}";
    return result;
  });

  if (!httpServer.Start(45678)) {
    // Non-fatal: log but don't show error (port might be in use)
    wxLogDebug("Failed to start HTTP server on port 45678");
  }

  // Center on screen
  Centre();
}

MainWindow::~MainWindow() {
  // Signal shutdown to prevent new background saves
  m_shuttingDown.store(true);

  // Clear callbacks first to prevent calls with dangling 'this'
  HttpServer::GetInstance().SetUrlCallback(nullptr);
  HttpServer::GetInstance().SetStatusCallback(nullptr);

  // Stop HTTP server and wait for threads
  HttpServer::GetInstance().Stop();

  // Stop timer to prevent callbacks during destruction
  if (m_updateTimer) {
    m_updateTimer->Stop();
    delete m_updateTimer;
    m_updateTimer = nullptr;
  }

  // Wait for any in-progress database save to complete
  {
    std::lock_guard<std::mutex> lock(m_dbSaveThreadMutex);
    if (m_dbSaveThread.joinable()) {
      m_dbSaveThread.join();
    }
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
  m_helpMenu->Append(ID_INSTALL_EXTENSION, "Install &Chrome Integration...",
                     "Install or repair the Chrome extension integration");
  m_helpMenu->AppendSeparator();
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
    iconPath.Normalize(wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE | wxPATH_NORM_TILDE);

    if (!wxFileExists(iconPath.GetFullPath())) {
      // Try ../resources
      iconPath = wxFileName(exePath);
      iconPath.SetFullName(name + ".png");
      iconPath.AppendDir("..");
      iconPath.AppendDir("resources");
      iconPath.Normalize(wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE | wxPATH_NORM_TILDE);
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
               "Version 2.0.0\n\n"
               "A powerful download manager built with wxWidgets, WinINet, and "
               "XML storage.\n\n"
               "Features:\n"
               "- Multi-threaded downloads\n"
               "- Pause/Resume support\n"
               "- Automatic file categorization\n"
               "- Download scheduling",
               "About LDM", wxOK | wxICON_INFORMATION, this);
}

void MainWindow::OnAddUrl(wxCommandEvent &event) {
  wxTextEntryDialog dialog(this,
                           "Enter the URL to download:", "Add New Download", "",
                           wxOK | wxCANCEL | wxCENTRE);

  if (dialog.ShowModal() == wxID_OK) {
    wxString url = dialog.GetValue();
    ProcessUrl(url);
  }
}

void MainWindow::ProcessUrl(const wxString &url, const wxString &referer) {
  if (!url.IsEmpty()) {
    try {
      // Debug: Log URL processing
      std::cout << "[MainWindow] ProcessUrl called with: " << url.ToStdString() << std::endl;
      if (!referer.IsEmpty()) {
        std::cout << "[MainWindow] Referer: " << referer.ToStdString() << std::endl;
      }
      std::cout.flush();

      // Check if this is a video site that needs yt-dlp
      std::cout << "[MainWindow] Getting YtDlpManager..." << std::endl;
      std::cout.flush();
      YtDlpManager &ytdlp = YtDlpManager::GetInstance();

      // Debug: Log video site detection and yt-dlp availability
      std::cout << "[MainWindow] Checking IsVideoSiteUrl..." << std::endl;
      std::cout.flush();
      bool isVideoSite = ytdlp.IsVideoSiteUrl(url.ToStdString());
      std::cout << "[MainWindow] Checking IsYtDlpAvailable..." << std::endl;
      std::cout.flush();
      bool ytdlpAvailable = ytdlp.IsYtDlpAvailable();
      std::cout << "[MainWindow] IsVideoSite: " << isVideoSite
                << ", YtDlpAvailable: " << ytdlpAvailable
                << ", Path: " << ytdlp.GetYtDlpPath() << std::endl;
      std::cout.flush();
      if (isVideoSite && !ytdlpAvailable) {
        // Prompt user to install yt-dlp
        int result = wxMessageBox(
            "This video site requires yt-dlp to download.\n\n"
            "yt-dlp is a free video downloader that supports 1400+ sites.\n"
            "Would you like to download it now? (~15 MB)",
            "Install yt-dlp?",
            wxYES_NO | wxICON_QUESTION, this);

        if (result == wxYES) {
          m_statusBar->SetStatusText("Downloading yt-dlp...", 0);

          // Show progress
          wxProgressDialog progressDlg(
              "Downloading yt-dlp",
              "Please wait while yt-dlp is being downloaded...",
              100, this,
              wxPD_APP_MODAL | wxPD_AUTO_HIDE);

          progressDlg.Pulse();

          std::atomic<bool> downloadComplete{false};
          std::atomic<bool> downloadSuccess{false};
          std::string downloadError;
          std::mutex downloadErrorMutex;

          ytdlp.DownloadYtDlp([&](bool success, const std::string &error) {
            downloadSuccess.store(success);
            {
              std::lock_guard<std::mutex> lock(downloadErrorMutex);
              downloadError = error;
            }
            downloadComplete.store(true);
          });

          // Wait for download to complete
          while (!downloadComplete.load()) {
            progressDlg.Pulse();
            wxMilliSleep(100);
            wxYield();
          }

          progressDlg.Close();

          if (downloadSuccess.load()) {
            m_statusBar->SetStatusText("yt-dlp installed! Checking ffmpeg...", 0);

            // Also download ffmpeg if not available (for high-quality video merging)
            if (!ytdlp.IsFfmpegAvailable()) {
              wxProgressDialog ffmpegDlg(
                  "Downloading ffmpeg",
                  "Downloading ffmpeg for high-quality video support...",
                  100, this,
                  wxPD_APP_MODAL | wxPD_AUTO_HIDE);

              ffmpegDlg.Pulse();

              std::atomic<bool> ffmpegComplete{false};
              std::atomic<bool> ffmpegSuccess{false};
              std::string ffmpegError;
              std::mutex ffmpegErrorMutex;

              ytdlp.DownloadFfmpeg([&](bool success, const std::string &error) {
                ffmpegSuccess.store(success);
                {
                  std::lock_guard<std::mutex> lock(ffmpegErrorMutex);
                  ffmpegError = error;
                }
                ffmpegComplete.store(true);
              });

              while (!ffmpegComplete.load()) {
                ffmpegDlg.Pulse();
                wxMilliSleep(100);
                wxYield();
              }

              ffmpegDlg.Close();

              if (ffmpegSuccess.load()) {
                // Also download Deno if not available (required for YouTube)
                if (!ytdlp.IsDenoAvailable()) {
                  m_statusBar->SetStatusText("Downloading Deno (JS runtime)...", 0);
                  wxProgressDialog denoDlg(
                      "Downloading Deno",
                      "Downloading JavaScript runtime for YouTube support...",
                      100, this,
                      wxPD_APP_MODAL | wxPD_AUTO_HIDE);

                  denoDlg.Pulse();

                  std::atomic<bool> denoComplete{false};
                  std::atomic<bool> denoSuccess{false};
                  std::string denoError;
                  std::mutex denoErrorMutex;

                  ytdlp.DownloadDeno([&](bool success, const std::string &error) {
                    denoSuccess.store(success);
                    {
                      std::lock_guard<std::mutex> lock(denoErrorMutex);
                      denoError = error;
                    }
                    denoComplete.store(true);
                  });

                  while (!denoComplete.load()) {
                    denoDlg.Pulse();
                    wxMilliSleep(100);
                    wxYield();
                  }

                  denoDlg.Close();

                  if (denoSuccess.load()) {
                    wxMessageBox("All components installed successfully!\n\nyt-dlp, ffmpeg, and Deno are ready.\nYour download will now start with best quality.",
                                 "Success", wxOK | wxICON_INFORMATION, this);
                  } else {
                    std::string denoErrorCopy;
                    {
                      std::lock_guard<std::mutex> lock(denoErrorMutex);
                      denoErrorCopy = denoError;
                    }
                    wxMessageBox("yt-dlp and ffmpeg installed!\n\nNote: Deno installation failed. YouTube may show warnings.\n\nError: " + denoErrorCopy,
                                 "Partial Success", wxOK | wxICON_WARNING, this);
                  }
                } else {
                  wxMessageBox("yt-dlp and ffmpeg installed successfully!\n\nYour download will now start with best quality.",
                               "Success", wxOK | wxICON_INFORMATION, this);
                }
              } else {
                std::string ffmpegErrorCopy;
                {
                  std::lock_guard<std::mutex> lock(ffmpegErrorMutex);
                  ffmpegErrorCopy = ffmpegError;
                }
                // ffmpeg failed but yt-dlp works - continue with reduced quality
                wxMessageBox("yt-dlp installed successfully!\n\nNote: ffmpeg installation failed. Videos will download in reduced quality.\n\nError: " + ffmpegErrorCopy,
                             "Partial Success", wxOK | wxICON_WARNING, this);
              }
            } else {
              wxMessageBox("yt-dlp installed successfully!\n\nYour download will now start.",
                           "Success", wxOK | wxICON_INFORMATION, this);
            }
          } else {
            std::string downloadErrorCopy;
            {
              std::lock_guard<std::mutex> lock(downloadErrorMutex);
              downloadErrorCopy = downloadError;
            }
            wxMessageBox(wxString::Format("Failed to install yt-dlp:\n%s", downloadErrorCopy),
                         "Error", wxOK | wxICON_ERROR, this);
            m_statusBar->SetStatusText("yt-dlp installation failed", 0);
            return;
          }
        } else {
          m_statusBar->SetStatusText("yt-dlp required for video downloads", 0);
          return;
        }
      }

      // Add download to the manager
      DownloadManager &manager = DownloadManager::GetInstance();
      int downloadId = manager.AddDownload(url.ToStdString());

      // Check for validation error
      if (downloadId < 0) {
        wxMessageBox("Invalid or unsupported URL.\n\nSupported: HTTP, HTTPS, FTP\nNot supported: blob:, data:, streaming (m3u8, mpd)",
                     "Invalid URL", wxOK | wxICON_ERROR, this);
        m_statusBar->SetStatusText("Invalid URL entered", 0);
        return;
      }

      // Get the download object and add to table
      auto download = manager.GetDownload(downloadId);
      if (download) {
        // Set referer if provided (for protected downloads)
        if (!referer.IsEmpty()) {
          download->SetReferer(referer.ToStdString());
          std::cout << "[MainWindow] Set referer on download: " << referer.ToStdString() << std::endl;
        }

        m_downloadsTable->AddDownload(download);

        // For video sites, show quality selection dialog
        // But skip dialog for problematic sites that tend to hang
        std::string urlStr = url.ToStdString();
        bool isVideoSite = ytdlp.IsVideoSiteUrl(urlStr);

        // Sites that should skip quality dialog (tend to hang or have issues)
        bool skipQualityDialog = (urlStr.find("xhamster.com") != std::string::npos ||
                                   urlStr.find("xvideos.com") != std::string::npos ||
                                   urlStr.find("xnxx.com") != std::string::npos ||
                                   urlStr.find("redtube.com") != std::string::npos);

        if (isVideoSite && !skipQualityDialog) {
          m_statusBar->SetStatusText("Fetching video info...", 0);
          wxBusyCursor wait;
          wxYield();

          // Get video title
          std::string videoTitle = ytdlp.GetVideoTitle(urlStr);
          if (videoTitle.empty()) {
            videoTitle = "Video";
          }

          // Show quality dialog
          VideoQualityDialog qualityDlg(this, urlStr, videoTitle);
          if (qualityDlg.ShowModal() == wxID_OK) {
            std::string formatId = qualityDlg.GetSelectedFormatId();
            manager.StartDownloadWithFormat(downloadId, formatId);
            m_statusBar->SetStatusText("Downloading: " + url, 0);
          } else {
            // User cancelled - remove the download
            manager.RemoveDownload(downloadId, false);
            m_downloadsTable->RemoveDownload(downloadId);
            m_statusBar->SetStatusText("Download cancelled", 0);
            return;
          }
        } else if (isVideoSite) {
          // Skip quality dialog for problematic sites - use default format
          m_statusBar->SetStatusText("Starting download (default quality)...", 0);
          manager.StartDownloadWithFormat(downloadId, ""); // Empty = default
          m_statusBar->SetStatusText("Downloading: " + url, 0);
        } else {
          // Start the download automatically for non-video files
          manager.StartDownload(downloadId);
          m_statusBar->SetStatusText("Downloading: " + url, 0);
        }

        m_statusBar->SetStatusText(
            wxString::Format("Downloads: %d", manager.GetTotalDownloads()), 1);
      }
    } catch (const std::exception& e) {
      wxMessageBox(wxString::Format("Error processing URL: %s", e.what()),
                   "Error", wxOK | wxICON_ERROR, this);
      m_statusBar->SetStatusText("Error processing URL", 0);
    } catch (...) {
      wxMessageBox("An unexpected error occurred while processing the URL.",
                   "Error", wxOK | wxICON_ERROR, this);
      m_statusBar->SetStatusText("Unexpected error", 0);
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

void MainWindow::OnStop(wxCommandEvent &event) {
  int selectedId = m_downloadsTable->GetSelectedDownloadId();
  if (selectedId >= 0) {
    DownloadManager::GetInstance().CancelDownload(selectedId);
    m_statusBar->SetStatusText("Download stopped", 0);
  } else {
    m_statusBar->SetStatusText("No download selected", 0);
  }
}

void MainWindow::OnStopAll(wxCommandEvent &event) {
  DownloadManager::GetInstance().CancelAllDownloads();
  m_statusBar->SetStatusText("All downloads stopped", 0);
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
  // Guard against callbacks during destruction
  if (!m_downloadsTable || !m_statusBar) {
    return;
  }
  
  // Refresh downloads table with latest data from DownloadManager
  DownloadManager &manager = DownloadManager::GetInstance();
  auto downloads = manager.GetAllDownloads();

  // Update each download in the table
  for (const auto &download : downloads) {
    m_downloadsTable->UpdateDownload(download->GetId());
  }

  // Periodic database save (every 60 ticks = 30 seconds at 500ms interval)
  // This ensures progress is saved in case of crash
  // Run in background thread to avoid UI blocking
  m_dbSaveCounter++;
  if (m_dbSaveCounter >= 60 && !m_shuttingDown.load() && !m_dbSaveInProgress.load()) {
    m_dbSaveCounter = 0;

    // Join previous save thread if it exists
    {
      std::lock_guard<std::mutex> lock(m_dbSaveThreadMutex);
      if (m_dbSaveThread.joinable()) {
        m_dbSaveThread.join();
      }

      m_dbSaveInProgress.store(true);
      m_dbSaveThread = std::thread([this]() {
        DownloadManager::GetInstance().SaveAllDownloadsToDatabase();
        DatabaseManager::GetInstance().Flush();
        m_dbSaveInProgress.store(false);
      });
    }
  }

  // Update status bar
  int active = manager.GetActiveDownloads();
  double speed = manager.GetTotalSpeed();

  if (active > 0) {
    m_statusBar->SetStatusText(wxString::Format("Downloading: %d", active), 0);
  } else {
    m_statusBar->SetStatusText("Ready", 0);
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

void MainWindow::OnInstallExtension(wxCommandEvent &event) {
  // Get current executable path
  wxString exePath = wxStandardPaths::Get().GetExecutablePath();
  wxFileName exeFileName(exePath);
  wxString exeDir = exeFileName.GetPath();
  
  // Find BrowserExtension folder
  wxString extPath = exeDir + "\\BrowserExtension";
  if (!wxDirExists(extPath)) {
      extPath = exeDir + "\\..\\BrowserExtension";
      if (!wxDirExists(extPath)) {
           extPath = exeDir + "\\..\\..\\BrowserExtension";
      }
  }
  
  if (!wxDirExists(extPath)) {
      extPath = exeDir + "\\..\\..\\..\\BrowserExtension";
  }
  
  // Normalize the path for display
  wxFileName extFileName(extPath);
  extFileName.Normalize(wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE);
  extPath = extFileName.GetFullPath();
  
  // Check if LDM's HTTP server is running
  HttpServer &server = HttpServer::GetInstance();
  wxString serverStatus = server.IsRunning() 
      ? wxString::Format("HTTP server running on port %d", server.GetPort())
      : "HTTP server not started (will start on next launch)";
  
  wxString message = wxString::Format(
      "Browser Extension Installation\n\n"
      "The new LDM browser extension uses HTTP communication - no registry setup needed!\n\n"
      "To install:\n"
      "1. Open Chrome/Edge Extensions page (chrome://extensions or edge://extensions)\n"
      "2. Enable 'Developer mode' (toggle in top-right)\n"
      "3. Click 'Load unpacked'\n"
      "4. Select this folder:\n   %s\n\n"
      "That's it! The extension will automatically connect to LDM when it's running.\n\n"
      "Status: %s",
      extPath, serverStatus);
  
  wxMessageBox(message, "Install Browser Extension", wxOK | wxICON_INFORMATION, this);
}

#ifdef _WIN32
WXLRESULT MainWindow::MSWWindowProc(WXUINT nMsg, WXWPARAM wParam, WXLPARAM lParam) {
  if (nMsg == WM_COPYDATA) {
    COPYDATASTRUCT *pCDS = (COPYDATASTRUCT *)lParam;
    if (pCDS->dwData == 1) { // 1 = URL data
      const char *url = (const char *)pCDS->lpData;
      if (url) {
        wxString urlStr = wxString::FromUTF8(url);
        // We need to process this on the main thread, but we are already on it.
        // However, bringing the window to front is important.
        if (IsIconized()) {
            Restore();
        }
        Show(true);
        Raise();
        RequestUserAttention();
        
        ProcessUrl(urlStr);
        return 0; // Handled
      }
    }
  }
  return wxFrame::MSWWindowProc(nMsg, wParam, lParam);
}
#endif

void MainWindow::OnGrabber(wxCommandEvent &event) {
  wxMessageBox(
      "URL Grabber\n\n"
      "The URL Grabber feature allows you to extract multiple download links "
      "from a webpage.\n\n"
      "This feature is planned for a future release.\n\n"
      "For now, you can:\n"
      "- Use the browser extension to send links directly to LDM\n"
      "- Drag and drop URLs onto the main window\n"
      "- Use Add URL (Ctrl+N) to add downloads manually",
      "URL Grabber", wxOK | wxICON_INFORMATION, this);
}
