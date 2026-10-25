// Composition root: the only place that knows every concrete type (design section 2).
#include "base/Utf.h"
#include "core/Coordinator.h"
#include "core/CoreThread.h"
#include "core/LaunchRequest.h"
#include "engine/CurlEngine.h"
#include "grabber/GrabberExplorer.h"
#include "platform/win/BrowserBridge.h"
#include "platform/win/WinBrowserWindows.h"
#include "platform/win/XmlLiteReader.h"
#include "platform/win/BrowserHostRegistration.h"
#include "platform/win/FfmpegMediaTools.h"
#include "platform/win/SiteToolsFolder.h"
#include "platform/win/WinToolInstaller.h"
#include "platform/win/YtDlpResolver.h"
#include "platform/win/InstanceChannel.h"
#include "platform/win/SystemPorts.h"
#include "platform/win/RunKeyStartup.h"
#include "platform/win/WinHttpProxyResolver.h"
#include "platform/win/WinShellActions.h"
#include "platform/win/WinSystemActions.h"
#include "storage/DpapiSecretProtector.h"
#include "storage/SqliteStore.h"
#include "ui/GrabberBridge.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/UiBridge.h"

#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kAppTitle[] = L"Last Download Manager";
// Design section 3, shutdown: how long running transfers get to acknowledge their stop.
constexpr std::chrono::seconds kShutdownTimeout{10};

std::optional<std::filesystem::path> KnownFolder(REFKNOWNFOLDERID id)
{
    PWSTR path = nullptr;
    if (FAILED(::SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &path))) {
        return std::nullopt;
    }
    std::filesystem::path result(path);
    ::CoTaskMemFree(path);
    return result;
}

std::filesystem::path ProgramFolder()
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return {};
        }
        if (length < path.size()) {
            path.resize(length);
            return std::filesystem::path(path).parent_path();
        }
        path.resize(path.size() * 2);
    }
}

void Fatal(const std::wstring& message)
{
    ::MessageBoxW(nullptr, message.c_str(), kAppTitle, MB_OK | MB_ICONERROR);
}

// Balanced COM and OLE initialization for the UI thread (file dialogs, drag and drop, clipboard).
class OleScope {
public:
    OleScope() : ok_(SUCCEEDED(::OleInitialize(nullptr))) {}
    ~OleScope()
    {
        if (ok_) {
            ::OleUninitialize();
        }
    }
    OleScope(const OleScope&) = delete;
    OleScope& operator=(const OleScope&) = delete;

private:
    bool ok_;
};

std::vector<std::wstring> CommandLineArguments()
{
    int count = 0;
    LPWSTR* raw = ::CommandLineToArgvW(::GetCommandLineW(), &count);
    if (raw == nullptr) {
        return {};
    }
    std::vector<std::wstring> arguments;
    for (int i = 1; i < count; ++i) {  // skip the program name
        arguments.emplace_back(raw[i]);
    }
    ::LocalFree(raw);
    return arguments;
}

int RunMessageLoop(ldm::ui::MainWindow& window)
{
    MSG message{};
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (window.PreTranslate(message)) {
            continue;
        }
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

}  // namespace

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int showCommand)
{
    ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);

    // Design section 7: arguments are validated before anything else happens, then a second
    // launch hands them to the running instance and exits.
    const ldm::core::CommandLineParse parse = ldm::core::ParseCommandLine(CommandLineArguments());
    if (parse.error) {
        Fatal(ldm::base::ToUtf16(*parse.error) + L"\n\n" + ldm::core::CommandLineHelp());
        return 2;
    }
    const ldm::core::LaunchRequest& launch = parse.request;
    if (launch.help) {
        ::MessageBoxW(nullptr, ldm::core::CommandLineHelp().c_str(), kAppTitle, MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    if (launch.writeHostManifests) {
        // The installer's last step: the browsers can start LDM before it was ever launched. No
        // window, no instance lock; LDM writes the same registration again on every start.
        const auto localData = KnownFolder(FOLDERID_LocalAppData);
        const std::filesystem::path programFolder = ProgramFolder();
        if (!localData || programFolder.empty()) {
            return 1;
        }
        return ldm::platform::RegisterBrowserHosts(*localData / L"Last Download Manager" / L"browser",
                                                   programFolder / L"ldm-host.exe")
                   ? 0
                   : 1;
    }
    const auto instanceLock = ldm::platform::InstanceLock::TryAcquire();
    if (!instanceLock) {
        ldm::platform::AllowRunningInstanceToActivate();
        if (!ldm::platform::ForwardToRunningInstance(launch)) {
            Fatal(L"Last Download Manager is already running but did not respond.");
            return 1;
        }
        return 0;
    }

    OleScope ole;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES | ICC_TAB_CLASSES |
                                                        ICC_PROGRESS_CLASS | ICC_UPDOWN_CLASS | ICC_LINK_CLASS |
                                                        ICC_DATE_CLASSES};
    ::InitCommonControlsEx(&controls);
    ldm::ui::Theme::InitializeProcess();

    const auto localData = KnownFolder(FOLDERID_LocalAppData);
    const auto downloads = KnownFolder(FOLDERID_Downloads);
    if (!localData || !downloads) {
        Fatal(L"The user folders could not be found.");
        return 1;
    }
    const std::filesystem::path dataFolder = *localData / L"Last Download Manager";
    std::error_code folderError;
    std::filesystem::create_directories(dataFolder, folderError);
    auto store = ldm::storage::SqliteStore::Open(ldm::base::ToUtf8((dataFolder / L"ldm.sqlite3").wstring()));
    if (!store) {
        Fatal(L"The download database could not be opened:\n" + ldm::base::ToUtf16(store.GetError().message) +
              L"\n\nIs Last Download Manager already running?");
        return 1;
    }

    // Construction order follows the data flow; destruction runs in reverse after an explicit,
    // ordered shutdown below.
    ldm::core::CoreThread core;
    ldm::ui::UiBridge bridge;
    ldm::ui::GrabberBridge grabberBridge;
    ldm::platform::SystemClock clock;
    ldm::platform::WinFileSystem fileSystem;
    ldm::storage::DpapiSecretProtector secrets;
    ldm::platform::RandomIdGenerator ids;
    ldm::platform::PowerRequest power;
    ldm::platform::WinTimeZone timeZone;
    ldm::platform::WaitableWakeTimer wakeTimer;
    ldm::platform::WaitableWakeTimer grabberWakeTimer;  // the Site Grabber's schedules have their own
    ldm::platform::WinHttpProxyResolver proxy(secrets, core);  // answers slow lookups through the core thread
    ldm::platform::WinShellActions shell;
    ldm::platform::WinSystemActions system;
    ldm::platform::RunKeyStartup startup;
    auto storeInstance = std::make_unique<ldm::storage::SqliteStore>(std::move(store).Value());
    auto engine = std::make_unique<ldm::engine::CurlEngine>(core);
    // ffmpeg.exe and ffprobe.exe ship next to ldm.exe; joins answer through the core thread.
    ldm::platform::FfmpegMediaTools mediaTools(ProgramFolder(), core);
    // yt-dlp and Deno for site videos live in the profile, downloaded when the user turns the
    // feature on; lookups and installs answer through the core thread.
    ldm::platform::SiteToolsFolder siteTools(dataFolder / L"tools", ProgramFolder());
    ldm::platform::YtDlpResolver siteResolver(siteTools.ResolverTools(), core, &siteTools.Lock());
    ldm::platform::WinToolInstaller toolInstaller(siteTools, core);

    // The Site Grabber runs on the core thread beside the coordinator (M4 design section 6.2) and
    // sees the coordinator's output first, taking what belongs to its own downloads.
    ldm::grabber::ExplorerPorts grabberPorts;
    grabberPorts.store = storeInstance.get();
    grabberPorts.engine = engine.get();
    grabberPorts.proxy = &proxy;
    grabberPorts.downloads = &core;
    grabberPorts.fileSystem = &fileSystem;
    grabberPorts.secrets = &secrets;
    grabberPorts.clock = &clock;
    grabberPorts.timeZone = &timeZone;
    grabberPorts.wakeTimer = &grabberWakeTimer;
    grabberPorts.observer = &grabberBridge;
    ldm::grabber::GrabberExplorer explorer(grabberPorts, ldm::grabber::ExplorerConfig{});
    ldm::grabber::CoreOutputTee coreOutput(explorer, bridge);

    ldm::core::CoordinatorPorts ports;
    ports.store = storeInstance.get();
    ports.engine = engine.get();
    ports.observer = &coreOutput;
    ports.clock = &clock;
    ports.fileSystem = &fileSystem;
    ports.secrets = &secrets;
    ports.ids = &ids;
    ports.power = &power;
    ports.proxy = &proxy;
    ports.media = &mediaTools;
    ports.timeZone = &timeZone;
    ports.wakeTimer = &wakeTimer;
    ports.sites = &siteResolver;
    ports.tools = &toolInstaller;
    ldm::core::CoordinatorConfig config;
    config.downloadsFolder = *downloads;
    // The update check (checklist X-05) asks the repository the README links, only when the user
    // turned it on.
    config.updateFeedUrl = "https://api.github.com/repos/xMags/Last-Download-Manager/releases/latest";
    config.appVersion = LDM_VERSION;
    ldm::core::Coordinator coordinator(ports, config);
    if (const auto started = core.Start(coordinator, &explorer); !started) {
        Fatal(L"Last Download Manager could not start:\n" + ldm::base::ToUtf16(started.GetError().message));
        return 1;
    }

    // Browser integration (M3 design section 3). Without it LDM still works; the extension then
    // shows that it cannot reach LDM.
    if (const auto programFolder = ProgramFolder(); !programFolder.empty()) {
        (void)ldm::platform::RegisterBrowserHosts(dataFolder / L"browser", programFolder / L"ldm-host.exe");
    }
    // Set before the bridge starts its thread, which is the only reader.
    ldm::ui::MainWindow* browserTarget = nullptr;
    ldm::platform::BrowserBridge browserBridge(
        [&browserTarget](ldm::ui::BrowserEvent event) { browserTarget->PostBrowserEvent(std::move(event)); });
    ldm::platform::WinBrowserWindows browserWindows;

    int exitCode = 0;
    {
        ldm::ui::MainWindow window(
            ldm::ui::MainWindow::Services{&core, &bridge, &shell, &system, &secrets, &startup, &browserBridge,
                                          &browserWindows, &ldm::platform::ReadXml,
                                          ldm::platform::IsProcessElevated(), &core, &grabberBridge});
        auto snapshot = bridge.TakeSnapshot();
        // --tray (Launch on startup) starts hidden, unless the tray icon is hidden too.
        const bool startHidden = launch.tray && snapshot && snapshot->settings.trayIconStyle != ldm::model::TrayIconStyle::Hidden;
        if (!window.Create(std::move(snapshot), startHidden ? SW_HIDE : showCommand)) {
            Fatal(L"The main window could not be created.");
        } else {
            if (launch.url) {
                window.PostLaunchRequest(launch);
            }
            // Declared after the window, so it stops before the window goes away. Without the pipe,
            // LDM still works; later launches then report that it did not respond.
            ldm::platform::InstanceServer server(
                [&window](ldm::core::LaunchRequest request) { window.PostLaunchRequest(std::move(request)); });
            (void)server.Start();
            browserTarget = &window;
            (void)browserBridge.Start();
            exitCode = RunMessageLoop(window);
            browserBridge.Stop();  // before the window it posts to goes away
        }
    }

    // Design section 3: stop transfers and checkpoint on the core thread, then the engine.
    core.Shutdown(kShutdownTimeout);
    engine.reset();
    storeInstance.reset();
    return exitCode;
}
