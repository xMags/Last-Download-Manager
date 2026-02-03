// Last Download Manager
// Main Entry Point

#include "ui/MainWindow.h"
#include <wx/image.h>
#include <wx/wx.h>

#ifdef _WIN32
#include <Windows.h>
#endif

class LastDMApp : public wxApp {
public:
  virtual bool OnInit() override {
    if (!wxApp::OnInit()) {
      return false;
    }

    // Initialize image handlers (required for PNG, JPEG, etc.)
    wxInitAllImageHandlers();

    // Enable high DPI support
#ifdef _WIN32
    SetProcessDPIAware();
#endif

    // Create and show main window
    MainWindow *mainWindow = new MainWindow();
    mainWindow->Show(true);

    return true;
  }
};

wxIMPLEMENT_APP(LastDMApp);
