// Composition root: the only place that knows every concrete type (design section 2).
#include "base/Utf.h"
#include "core/Coordinator.h"
#include "core/CoreThread.h"
#include "engine/CurlEngine.h"
#include "platform/win/SystemPorts.h"
#include "platform/win/WinHttpProxyResolver.h"
#include "platform/win/WinShellActions.h"
#include "storage/DpapiSecretProtector.h"
#include "storage/SqliteStore.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/UiBridge.h"

#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <shlobj.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

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
    OleScope ole;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES | ICC_TAB_CLASSES |
                                                        ICC_PROGRESS_CLASS | ICC_UPDOWN_CLASS | ICC_LINK_CLASS};
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
    ldm::platform::SystemClock clock;
    ldm::platform::WinFileSystem fileSystem;
    ldm::storage::DpapiSecretProtector secrets;
    ldm::platform::RandomIdGenerator ids;
    ldm::platform::PowerRequest power;
    ldm::platform::WinHttpProxyResolver proxy(secrets);
    ldm::platform::WinShellActions shell;
    auto storeInstance = std::make_unique<ldm::storage::SqliteStore>(std::move(store).Value());
    auto engine = std::make_unique<ldm::engine::CurlEngine>(core);

    ldm::core::CoordinatorPorts ports;
    ports.store = storeInstance.get();
    ports.engine = engine.get();
    ports.observer = &bridge;
    ports.clock = &clock;
    ports.fileSystem = &fileSystem;
    ports.secrets = &secrets;
    ports.ids = &ids;
    ports.power = &power;
    ports.proxy = &proxy;
    ldm::core::CoordinatorConfig config;
    config.downloadsFolder = *downloads;
    ldm::core::Coordinator coordinator(ports, config);
    if (const auto started = core.Start(coordinator); !started) {
        Fatal(L"Last Download Manager could not start:\n" + ldm::base::ToUtf16(started.GetError().message));
        return 1;
    }

    int exitCode = 0;
    {
        ldm::ui::MainWindow window(ldm::ui::MainWindow::Services{&core, &bridge, &shell});
        if (!window.Create(bridge.TakeSnapshot(), showCommand)) {
            Fatal(L"The main window could not be created.");
        } else {
            exitCode = RunMessageLoop(window);
        }
    }

    // Design section 3: stop transfers and checkpoint on the core thread, then the engine.
    core.Shutdown(kShutdownTimeout);
    engine.reset();
    storeInstance.reset();
    return exitCode;
}
