// ldm-host.exe end to end (M3 design section 3): the test plays the browser on the host's stdio
// and LDM on a private browser pipe, so the user's running LDM is never involved.
#include "browser/HostManifest.h"
#include "browser/Protocol.h"
#include "platform/win/BrowserBridge.h"
#include "platform/win/PipeIo.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace {

using ldm::platform::OwnedHandle;

constexpr std::chrono::seconds kWait{10};

enum class HostExit : DWORD {
    Done = 0,
    NotStartedByBrowser = 2,
    NoLdm = 3,
    ProtocolError = 4,
};

// What the extension sends first on every connection. Never asks to start LDM here: that would
// start the real ldm.exe next to the host.
constexpr char kHello[] =
    R"({"v":1,"id":"h1","type":"hello","browser":"Chrome","browserVersion":"141.0","extensionVersion":"1.0.0","incognitoAllowed":true,"startLdm":false})";

std::wstring ChromiumOrigin()
{
    return L"chrome-extension://" + std::wstring(ldm::browser::kChromiumExtensionId.begin(),
                                                 ldm::browser::kChromiumExtensionId.end()) + L"/";
}

// Events from the bridge, collected for the test thread.
class EventQueue {
public:
    void Push(ldm::ui::BrowserEvent event)
    {
        {
            const std::lock_guard lock(mutex_);
            events_.push_back(std::move(event));
        }
        changed_.notify_all();
    }

    std::optional<ldm::ui::BrowserEvent> Pop()
    {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, kWait, [this] { return !events_.empty(); })) {
            return std::nullopt;
        }
        ldm::ui::BrowserEvent event = std::move(events_.front());
        events_.pop_front();
        return event;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<ldm::ui::BrowserEvent> events_;
};

class BrowserHostTest : public testing::Test {
protected:
    void SetUp() override
    {
        static int counter = 0;
        const std::wstring suffix = L"test" + std::to_wstring(::GetCurrentProcessId()) + L"x" + std::to_wstring(++counter);
        ASSERT_TRUE(::SetEnvironmentVariableW(L"LDM_PIPE_SUFFIX", suffix.c_str()));
        bridge_ = std::make_unique<ldm::platform::BrowserBridge>([this](ldm::ui::BrowserEvent event) {
            events_.Push(std::move(event));
        });
        ASSERT_TRUE(bridge_->Start());
    }

    void TearDown() override
    {
        if (host_.Valid()) {
            ::TerminateProcess(host_.Get(), 1);
            ::WaitForSingleObject(host_.Get(), INFINITE);
        }
        bridge_.reset();
        ::SetEnvironmentVariableW(L"LDM_PIPE_SUFFIX", nullptr);
    }

    // Starts the host with stdio pipes, as a browser does.
    void StartHost(const std::wstring& arguments)
    {
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        HANDLE childInput = nullptr;
        HANDLE parentInput = nullptr;
        HANDLE parentOutput = nullptr;
        HANDLE childOutput = nullptr;
        ASSERT_TRUE(::CreatePipe(&childInput, &parentInput, &inherit, 0));
        ASSERT_TRUE(::CreatePipe(&parentOutput, &childOutput, &inherit, 0));
        OwnedHandle childIn(childInput);
        OwnedHandle childOut(childOutput);
        toHost_ = OwnedHandle(parentInput);
        fromHost_ = OwnedHandle(parentOutput);
        ::SetHandleInformation(toHost_.Get(), HANDLE_FLAG_INHERIT, 0);
        ::SetHandleInformation(fromHost_.Get(), HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW startup{sizeof(startup)};
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = childIn.Get();
        startup.hStdOutput = childOut.Get();
        startup.hStdError = childOut.Get();
        std::wstring commandLine = L"\"" LDM_HOST_EXE L"\" " + arguments;
        PROCESS_INFORMATION process{};
        ASSERT_TRUE(::CreateProcessW(LDM_HOST_EXE, commandLine.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                                     &startup, &process));
        ::CloseHandle(process.hThread);
        host_ = OwnedHandle(process.hProcess);
    }

    void SendToHost(const std::string& json)
    {
        unsigned char header[4] = {};
        ldm::platform::EncodeFrameLength(static_cast<std::uint32_t>(json.size()), header);
        DWORD written = 0;
        ASSERT_TRUE(::WriteFile(toHost_.Get(), header, 4, &written, nullptr));
        ASSERT_TRUE(::WriteFile(toHost_.Get(), json.data(), static_cast<DWORD>(json.size()), &written, nullptr));
    }

    // Reads exactly `size` bytes from the host's stdout, polling so a silent host fails the test
    // instead of hanging it.
    bool ReadFromHost(void* data, DWORD size)
    {
        auto* bytes = static_cast<char*>(data);
        DWORD done = 0;
        const auto deadline = std::chrono::steady_clock::now() + kWait;
        while (done < size) {
            DWORD available = 0;
            if (!::PeekNamedPipe(fromHost_.Get(), nullptr, 0, nullptr, &available, nullptr)) {
                return false;
            }
            if (available == 0) {
                if (std::chrono::steady_clock::now() > deadline) {
                    return false;
                }
                ::Sleep(10);
                continue;
            }
            DWORD chunk = 0;
            if (!::ReadFile(fromHost_.Get(), bytes + done, std::min(available, size - done), &chunk, nullptr)) {
                return false;
            }
            done += chunk;
        }
        return true;
    }

    std::optional<nlohmann::json> ReceiveFromHost()
    {
        unsigned char header[4] = {};
        if (!ReadFromHost(header, 4)) {
            return std::nullopt;
        }
        std::string json(ldm::platform::DecodeFrameLength(header), '\0');
        if (!ReadFromHost(json.data(), static_cast<DWORD>(json.size()))) {
            return std::nullopt;
        }
        return nlohmann::json::parse(json, nullptr, false);
    }

    std::optional<DWORD> WaitForHostExit()
    {
        if (::WaitForSingleObject(host_.Get(), static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(kWait).count())) != WAIT_OBJECT_0) {
            return std::nullopt;
        }
        DWORD code = 0;
        ::GetExitCodeProcess(host_.Get(), &code);
        host_.Reset();
        return code;
    }

    // The host's introduction, which every connection starts with.
    ldm::ui::BrowserConnectionId ExpectHostInfo()
    {
        const auto event = events_.Pop();
        EXPECT_TRUE(event && event->message);
        if (!event || !event->message) {
            return 0;
        }
        const auto* info = std::get_if<ldm::browser::HostInfo>(&event->message->message);
        EXPECT_NE(info, nullptr);
        if (info != nullptr) {
            EXPECT_EQ(info->family, ldm::browser::BrowserFamily::Chromium);
            EXPECT_EQ(info->extensionId, ldm::browser::kChromiumExtensionId);
        }
        return event->connection;
    }

    EventQueue events_;
    std::unique_ptr<ldm::platform::BrowserBridge> bridge_;
    OwnedHandle host_;
    OwnedHandle toHost_;
    OwnedHandle fromHost_;
};

TEST_F(BrowserHostTest, HelloIsAnsweredWithConfigAndDisconnectIsReported)
{
    StartHost(ChromiumOrigin() + L" --parent-window=0");
    SendToHost(kHello);
    const auto connection = ExpectHostInfo();
    ASSERT_NE(connection, 0u);
    const auto hello = events_.Pop();
    ASSERT_TRUE(hello && hello->message);
    EXPECT_EQ(hello->connection, connection);
    EXPECT_EQ(hello->message->id, "h1");
    ASSERT_TRUE(std::holds_alternative<ldm::browser::Hello>(hello->message->message));
    EXPECT_TRUE(std::get<ldm::browser::Hello>(hello->message->message).incognitoAllowed);

    bridge_->Send(connection, ldm::browser::EncodeConfig({"5.0.0", {"zip", "r0*"}}, "h1"));
    const auto config = ReceiveFromHost();
    ASSERT_TRUE(config && config->is_object());
    EXPECT_EQ((*config)["type"], "config");
    EXPECT_EQ((*config)["id"], "h1");
    EXPECT_EQ((*config)["capturedExtensions"], nlohmann::json({"zip", "r0*"}));

    // The browser closes stdin when the extension disconnects.
    toHost_.Reset();
    EXPECT_EQ(WaitForHostExit(), static_cast<DWORD>(HostExit::Done));
    const auto closed = events_.Pop();
    ASSERT_TRUE(closed);
    EXPECT_EQ(closed->connection, connection);
    EXPECT_FALSE(closed->message);
}

TEST_F(BrowserHostTest, HostRefusesMalformedMessagesFromTheBrowser)
{
    StartHost(ChromiumOrigin());
    SendToHost(kHello);
    const auto connection = ExpectHostInfo();
    ASSERT_TRUE(events_.Pop());  // the hello
    SendToHost(R"({"v":2,"id":"x","type":"hello"})");
    EXPECT_EQ(WaitForHostExit(), static_cast<DWORD>(HostExit::ProtocolError));
    const auto closed = events_.Pop();
    ASSERT_TRUE(closed);
    EXPECT_EQ(closed->connection, connection);
    EXPECT_FALSE(closed->message);
}

TEST_F(BrowserHostTest, BridgeClosesConnectionsThatSendUnknownTypes)
{
    StartHost(ChromiumOrigin());
    SendToHost(kHello);
    const auto connection = ExpectHostInfo();
    ASSERT_TRUE(events_.Pop());  // the hello
    // A valid envelope the host relays, but LDM does not know the type.
    SendToHost(R"({"v":1,"id":"x","type":"format-disk"})");
    const auto closed = events_.Pop();
    ASSERT_TRUE(closed);
    EXPECT_EQ(closed->connection, connection);
    EXPECT_FALSE(closed->message);
    EXPECT_TRUE(WaitForHostExit().has_value());
}

TEST_F(BrowserHostTest, HostStartedWithoutABrowserOriginExits)
{
    StartHost(L"");
    EXPECT_EQ(WaitForHostExit(), static_cast<DWORD>(HostExit::NotStartedByBrowser));
    StartHost(L"chrome-extension://aaaa/");
    EXPECT_EQ(WaitForHostExit(), static_cast<DWORD>(HostExit::NotStartedByBrowser));
}

TEST_F(BrowserHostTest, MalformedFirstMessageNeverReachesLdm)
{
    StartHost(ChromiumOrigin());
    SendToHost("not json");
    EXPECT_EQ(WaitForHostExit(), static_cast<DWORD>(HostExit::ProtocolError));
}

TEST_F(BrowserHostTest, HostDoesNotStartLdmUnlessAsked)
{
    bridge_.reset();  // nothing listens on the pipe now
    StartHost(ChromiumOrigin());
    SendToHost(kHello);
    EXPECT_EQ(WaitForHostExit(), static_cast<DWORD>(HostExit::NoLdm));
}

TEST_F(BrowserHostTest, ConnectionsAreNumberedAndKeptApart)
{
    StartHost(ChromiumOrigin());
    SendToHost(kHello);
    const auto first = ExpectHostInfo();
    ASSERT_TRUE(events_.Pop());  // the hello
    toHost_.Reset();
    ASSERT_TRUE(WaitForHostExit().has_value());
    ASSERT_TRUE(events_.Pop());  // closed

    StartHost(ChromiumOrigin());
    SendToHost(kHello);
    const auto second = ExpectHostInfo();
    ASSERT_TRUE(events_.Pop());  // the hello
    EXPECT_GT(second, first);
    // A late answer for the closed connection must not reach the new one.
    bridge_->Send(first, ldm::browser::EncodeConfig({"5.0.0", {}}, "stale"));
    bridge_->Send(second, ldm::browser::EncodeConfig({"5.0.0", {}}, "fresh"));
    const auto config = ReceiveFromHost();
    ASSERT_TRUE(config && config->is_object());
    EXPECT_EQ((*config)["id"], "fresh");
}

}  // namespace
