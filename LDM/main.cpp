// LDM
// Main Entry Point

#include "ui/MainWindow.h"
#include <wx/image.h>
#include <wx/wx.h>
#include <wx/cmdline.h>
#include <iostream>

#ifdef _WIN32
#include <Windows.h>
#endif

#include <wx/snglinst.h>

class LDMApp : public wxApp {
public:
  virtual bool OnInit() override {
    // Disable wxWidgets automatic command line parsing which treats arguments starting with - or / as options
    // and others as input files, often causing "Unexpected parameter" errors.
    // We handle arguments manually via argc/argv.
    wxApp::SetAppName("LDM");
    
    if (!wxApp::OnInit()) {
      return false;
    }

    // Check for existing instance
    m_singleInstanceChecker = new wxSingleInstanceChecker;
    if (m_singleInstanceChecker->IsAnotherRunning()) {
        // Another instance is running. Check if we have arguments (URL) to pass.
        if (argc > 1) {
            wxString url = argv[1];
            SendUrlToExistingInstance(url);
        }
        return false; // Exit this instance
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

    // If we were started with an argument, process it immediately
    if (argc > 1) {
        mainWindow->ProcessUrl(argv[1]);
    }

    return true;
  }
  
  // Override OnCmdLineParsed to prevent default handling? 
  // Actually, easiest way is to override OnInitCmdLine and do nothing or just return true.
  virtual void OnInitCmdLine(wxCmdLineParser& parser) override {
      // By default wxApp adds standard switches like --help, --verbose.
      // We want to suppress error messages for unknown parameters which might be our URL.
      // But standard wxApp::OnInit calls Parse() which triggers the error.
      
      // We can clear the parser and add just what we expect, or simply tell it to ignore everything.
      parser.SetDesc(g_cmdLineDesc);
      parser.SetSwitchChars("-"); // standard unix-style
  }

  virtual bool OnCmdLineParsed(wxCmdLineParser& parser) override {
      // We don't rely on the parser for the URL, we access argv directly in OnInit.
      // This override just keeps the base class happy.
      return true;
  }

private:
    static const wxCmdLineEntryDesc g_cmdLineDesc[];
  
  virtual int OnExit() override {
      delete m_singleInstanceChecker;
      return wxApp::OnExit();
  }

private:
    wxSingleInstanceChecker* m_singleInstanceChecker;

    void SendUrlToExistingInstance(const wxString& url) {
#ifdef _WIN32
        // Find the main window of the existing instance
        // Search for any window with "Last Download Manager" in the title (version-independent)
        HWND hWnd = NULL;
        HWND hSearch = ::GetTopWindow(NULL);
        while (hSearch) {
            char title[256];
            ::GetWindowTextA(hSearch, title, sizeof(title));
            if (strstr(title, "Last Download Manager") != NULL) {
                hWnd = hSearch;
                break;
            }
            hSearch = ::GetNextWindow(hSearch, GW_HWNDNEXT);
        }

        if (hWnd) {
            // Send WM_COPYDATA
            std::string urlStd = url.ToStdString();
            COPYDATASTRUCT cds;
            cds.dwData = 1; // 1 indicates URL
            cds.cbData = (DWORD)(urlStd.length() + 1);
            cds.lpData = (PVOID)urlStd.c_str();

            SendMessageA(hWnd, WM_COPYDATA, (WPARAM)NULL, (LPARAM)&cds);

            // Allow time for processing
            Sleep(100);
        }
#endif
    }
};

const wxCmdLineEntryDesc LDMApp::g_cmdLineDesc[] = {
    { wxCMD_LINE_PARAM, NULL, NULL, "url", wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL },
    { wxCMD_LINE_NONE }
};

wxIMPLEMENT_APP_NO_MAIN(LDMApp);

#if defined(_WIN32) || defined(WIN32)
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // Check for debug mode:
    // 1. --debug or -d command line flag
    // 2. debug.txt file exists next to exe (for portable apps)
    bool enableDebug = false;

    std::string cmdLine(lpCmdLine ? lpCmdLine : "");
    if (cmdLine.find("--debug") != std::string::npos || cmdLine.find("-d") != std::string::npos) {
        enableDebug = true;
    }

    // Check for debug.txt next to exe
    if (!enableDebug) {
        char exePath[MAX_PATH];
        if (GetModuleFileNameA(NULL, exePath, MAX_PATH)) {
            std::string debugFilePath(exePath);
            size_t lastSlash = debugFilePath.rfind('\\');
            if (lastSlash != std::string::npos) {
                debugFilePath = debugFilePath.substr(0, lastSlash + 1) + "debug.txt";
                DWORD attrs = GetFileAttributesA(debugFilePath.c_str());
                if (attrs != INVALID_FILE_ATTRIBUTES) {
                    enableDebug = true;
                }
            }
        }
    }

    if (enableDebug) {
        AllocConsole();
        FILE* fDummy;
        freopen_s(&fDummy, "CONOUT$", "w", stdout);
        freopen_s(&fDummy, "CONOUT$", "w", stderr);
        std::cout << "[LDM] Debug console enabled" << std::endl;
    }

    return wxEntry(hInstance, hPrevInstance, lpCmdLine, nCmdShow);
}
#else
int main(int argc, char* argv[]) {
    return wxEntry(argc, argv);
}
#endif
