#pragma once

#include "CategoriesPanel.h"
#include "DownloadsTable.h"
#include "SpeedGraphPanel.h"
#include <wx/artprov.h>
#include <wx/splitter.h>
#include <wx/taskbar.h>
#include <wx/timer.h>
#include <wx/toolbar.h>
#include <wx/wx.h>
#include <atomic>
#include <thread>
#include <mutex>

// Forward declaration for system tray
class LDMTaskBarIcon;

class MainWindow : public wxFrame {
  friend class LDMTaskBarIcon; // Allow tray icon to access ShowFromTray()
public:
  MainWindow();
  ~MainWindow();

  // Helper to process URL (used by DnD and browser extension)
  void ProcessUrl(const wxString &url, const wxString &referer = wxEmptyString);

private:
  // UI Components
  wxSplitterWindow *m_splitter;
  CategoriesPanel *m_categoriesPanel;
  DownloadsTable *m_downloadsTable;
  wxToolBar *m_toolbar;
  wxStatusBar *m_statusBar;
  wxTimer *m_updateTimer;
  SpeedGraphPanel *m_speedGraph;

  // System tray
  LDMTaskBarIcon *m_taskBarIcon;
  bool m_minimizedToTray = false;
  bool m_isClosing = false;
  
  // Periodic database save counter (every 60 ticks = 30 seconds at 500ms interval)
  int m_dbSaveCounter = 0;

  // Background save thread management
  std::atomic<bool> m_dbSaveInProgress{false};
  std::atomic<bool> m_shuttingDown{false};
  std::thread m_dbSaveThread;
  std::mutex m_dbSaveThreadMutex;

  // Menu bar
  wxMenuBar *m_menuBar;
  wxMenu *m_fileMenu;
  wxMenu *m_tasksMenu;
  wxMenu *m_downloadsMenu;
  wxMenu *m_viewMenu;
  wxMenu *m_helpMenu;

  // Initialization methods
  void CreateMenuBar();
  void CreateToolBar();
  void CreateStatusBar();
  void CreateMainContent();

  // Event handlers
  void OnExit(wxCommandEvent &event);
  void OnAbout(wxCommandEvent &event);
  void OnAddUrl(wxCommandEvent &event);
  void OnResume(wxCommandEvent &event);
  void OnPause(wxCommandEvent &event);
  void OnStop(wxCommandEvent &event);
  void OnStopAll(wxCommandEvent &event);
  void OnDelete(wxCommandEvent &event);
  void OnOptions(wxCommandEvent &event);
  void OnScheduler(wxCommandEvent &event);
  void OnStartQueue(wxCommandEvent &event);
  void OnStopQueue(wxCommandEvent &event);
  void OnViewDarkMode(wxCommandEvent &event);
  void OnCategorySelected(wxTreeEvent &event);
  void OnUpdateTimer(wxTimerEvent &event);
  void OnInstallExtension(wxCommandEvent &event);
  void OnGrabber(wxCommandEvent &event);

  // System tray handlers
  void OnIconize(wxIconizeEvent &event);
  void OnClose(wxCloseEvent &event);
  void ShowFromTray();
  void ShowNotification(const wxString &title, const wxString &message);
  void OnTrayExitRequest();
  void CleanupEventHandlers();
  void EnsureTrayIcon();

#ifdef _WIN32
  // Override MSWWindowProc to handle WM_COPYDATA for single instance URL passing
  virtual WXLRESULT MSWWindowProc(WXUINT nMsg, WXWPARAM wParam, WXLPARAM lParam) override;
#endif

  wxDECLARE_EVENT_TABLE();
};

// Menu and toolbar IDs
enum {
  ID_ADD_URL = wxID_HIGHEST + 1,
  ID_RESUME,
  ID_PAUSE,
  ID_STOP,
  ID_STOP_ALL,
  ID_DELETE,
  ID_DELETE_COMPLETED,
  ID_OPTIONS,
  ID_SCHEDULER,
  ID_START_QUEUE,
  ID_STOP_QUEUE,
  ID_GRABBER,
  ID_TELL_FRIEND,
  ID_CATEGORIES_PANEL,
  ID_DOWNLOADS_TABLE,
  ID_VIEW_DARK_MODE,
  ID_UPDATE_TIMER,
  ID_INSTALL_EXTENSION,
  ID_TRAY_SHOW,
  ID_TRAY_EXIT
};

// System tray icon class
class LDMTaskBarIcon : public wxTaskBarIcon {
public:
  LDMTaskBarIcon(MainWindow *parent) : m_parent(parent) {}

  wxMenu *CreatePopupMenu() override {
    wxMenu *menu = new wxMenu;
    menu->Append(ID_TRAY_SHOW, wxT("Show LDM"));
    menu->AppendSeparator();
    menu->Append(ID_TRAY_EXIT, wxT("Exit"));
    return menu;
  }

  void OnTrayShow(wxCommandEvent &event);
  void OnTrayExit(wxCommandEvent &event);
  void OnLeftClick(wxTaskBarIconEvent &event);

private:
  MainWindow *m_parent;

  wxDECLARE_EVENT_TABLE();
};
