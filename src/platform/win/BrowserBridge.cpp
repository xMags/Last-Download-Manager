#include "platform/win/BrowserBridge.h"

#include "platform/win/PipeIo.h"

#include <array>
#include <vector>

namespace ldm::platform {

namespace {

constexpr DWORD kPipeBufferBytes = 64 * 1024;
constexpr DWORD kWriteTimeoutMs = 3000;

}  // namespace

// One pipe instance and the read in progress on it. Lives on the bridge thread only.
struct BrowserBridge::Slot {
    enum class Phase {
        Dead,       // Windows refused to listen; the slot stays unused
        Listening,  // waiting for ldm-host.exe to connect
        Header,     // reading a 4-byte length
        Body,       // reading a message
    };

    OwnedHandle pipe;
    OwnedHandle event;  // manual reset; signaled when the pending operation completes
    OVERLAPPED overlapped{};
    Phase phase = Phase::Dead;
    bool connectedAtOnce = false;  // the client connected before ConnectNamedPipe was called
    // An overlapped operation is in flight, so Close must cancel it and wait for it. Only set when
    // Windows said ERROR_IO_PENDING: a call that fails at once leaves `overlapped` looking pending
    // and never signals the event, so waiting for it would hang the bridge.
    bool pending = false;
    ui::BrowserConnectionId connection = 0;
    bool introduced = false;  // the host message arrived
    unsigned char header[4] = {};
    std::string body;
    DWORD have = 0;  // bytes of the header or body read so far
};

BrowserBridge::BrowserBridge(std::function<void(ui::BrowserEvent)> onEvent) : onEvent_(std::move(onEvent)) {}

BrowserBridge::~BrowserBridge()
{
    Stop();
    if (wakeEvent_ != nullptr) {
        ::CloseHandle(wakeEvent_);
    }
    if (stopEvent_ != nullptr) {
        ::CloseHandle(stopEvent_);
    }
}

bool BrowserBridge::Start()
{
    stopEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    wakeEvent_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    // A host below LDM's level cannot connect: an LDM running as administrator then refuses the
    // browsers, as its elevation warning tells the user, instead of taking commands from any
    // program at normal level.
    OwnerOnlySecurity security(LowerIntegrity::Refused);
    if (stopEvent_ == nullptr || wakeEvent_ == nullptr || !security.Valid()) {
        return false;
    }
    const std::wstring name = BrowserPipeName();
    for (int i = 0; i < kMaxConnections; ++i) {
        auto slot = std::make_unique<Slot>();
        // The first instance claims the name, so no other program can pose as LDM's browser pipe.
        const DWORD openMode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (i == 0 ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
        slot->pipe = OwnedHandle(::CreateNamedPipeW(name.c_str(), openMode,
                                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                                    kMaxConnections, kPipeBufferBytes, kPipeBufferBytes, 0,
                                                    security.Attributes()));
        slot->event = OwnedHandle(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!slot->pipe.Valid() || !slot->event.Valid()) {
            slots_.clear();
            return false;
        }
        slots_.push_back(std::move(slot));
    }
    thread_ = std::thread([this] { Run(); });
    return true;
}

void BrowserBridge::Stop()
{
    if (stopEvent_ != nullptr) {
        ::SetEvent(stopEvent_);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    slots_.clear();
}

void BrowserBridge::Send(ui::BrowserConnectionId connection, std::string json)
{
    {
        const std::lock_guard lock(mutex_);
        outbox_.emplace_back(connection, std::move(json));
    }
    if (wakeEvent_ != nullptr) {
        ::SetEvent(wakeEvent_);
    }
}

void BrowserBridge::Listen(Slot& slot)
{
    slot.overlapped = OVERLAPPED{};
    slot.overlapped.hEvent = slot.event.Get();
    ::ResetEvent(slot.event.Get());
    slot.connectedAtOnce = false;
    slot.pending = false;
    slot.phase = Slot::Phase::Listening;
    if (::ConnectNamedPipe(slot.pipe.Get(), &slot.overlapped)) {
        return;  // completed; the event is set
    }
    if (::GetLastError() == ERROR_NO_DATA) {
        // A client connected and left before we listened: clear the instance and listen again.
        ::DisconnectNamedPipe(slot.pipe.Get());
        if (::ConnectNamedPipe(slot.pipe.Get(), &slot.overlapped)) {
            return;
        }
    }
    switch (::GetLastError()) {
    case ERROR_IO_PENDING:
        slot.pending = true;
        return;
    case ERROR_PIPE_CONNECTED:
        slot.connectedAtOnce = true;
        ::SetEvent(slot.event.Get());
        return;
    default:
        slot.phase = Slot::Phase::Dead;
        return;
    }
}

bool BrowserBridge::BeginRead(Slot& slot)
{
    slot.overlapped = OVERLAPPED{};
    slot.overlapped.hEvent = slot.event.Get();
    ::ResetEvent(slot.event.Get());
    void* base = slot.phase == Slot::Phase::Header ? static_cast<void*>(slot.header) : slot.body.data();
    char* target = static_cast<char*>(base);
    const DWORD size = slot.phase == Slot::Phase::Header ? 4 : static_cast<DWORD>(slot.body.size());
    // On an overlapped handle the event is set whether the read completes now or later; a read
    // that fails at once (the host already left) sets nothing.
    if (::ReadFile(slot.pipe.Get(), target + slot.have, size - slot.have, nullptr, &slot.overlapped)) {
        slot.pending = false;
        return true;
    }
    slot.pending = ::GetLastError() == ERROR_IO_PENDING;
    return slot.pending;
}

void BrowserBridge::Close(Slot& slot, bool notify)
{
    if (slot.pending) {
        ::CancelIoEx(slot.pipe.Get(), &slot.overlapped);
        DWORD ignored = 0;
        ::GetOverlappedResult(slot.pipe.Get(), &slot.overlapped, &ignored, TRUE);
        slot.pending = false;
    }
    if (notify && slot.introduced) {
        onEvent_(ui::BrowserEvent{slot.connection, std::nullopt});
    }
    ::DisconnectNamedPipe(slot.pipe.Get());
    slot.connection = 0;
    slot.introduced = false;
    slot.body.clear();
    slot.have = 0;
    slot.phase = Slot::Phase::Dead;
}

// A complete message arrived; false closes the connection.
bool BrowserBridge::Deliver(Slot& slot)
{
    auto parsed = browser::ParseMessage(slot.body);
    if (!parsed) {
        return false;
    }
    const bool isHostInfo = std::holds_alternative<browser::HostInfo>(parsed->message);
    // The host introduces itself exactly once, before anything else.
    if (isHostInfo == slot.introduced) {
        return false;
    }
    slot.introduced = true;
    onEvent_(ui::BrowserEvent{slot.connection, std::move(*parsed)});
    return true;
}

// The slot's pending operation finished; advance it. False closes the connection.
bool BrowserBridge::Advance(Slot& slot)
{
    slot.pending = false;  // its event fired: the operation is complete
    if (slot.phase == Slot::Phase::Listening) {
        DWORD ignored = 0;
        if (!slot.connectedAtOnce && !::GetOverlappedResult(slot.pipe.Get(), &slot.overlapped, &ignored, FALSE)) {
            return false;
        }
        slot.connection = ++lastConnection_;
        slot.phase = Slot::Phase::Header;
        slot.have = 0;
        return BeginRead(slot);
    }
    DWORD read = 0;
    if (!::GetOverlappedResult(slot.pipe.Get(), &slot.overlapped, &read, FALSE) || read == 0) {
        return false;  // the host exited or broke the pipe
    }
    slot.have += read;
    if (slot.phase == Slot::Phase::Header && slot.have == 4) {
        const std::uint32_t length = DecodeFrameLength(slot.header);
        if (length == 0 || length > browser::kMaxInboundBytes) {
            return false;
        }
        slot.body.assign(length, '\0');
        slot.phase = Slot::Phase::Body;
        slot.have = 0;
    } else if (slot.phase == Slot::Phase::Body && slot.have == slot.body.size()) {
        if (!Deliver(slot)) {
            return false;
        }
        slot.body.clear();
        slot.phase = Slot::Phase::Header;
        slot.have = 0;
    }
    return BeginRead(slot);
}

void BrowserBridge::FlushOutbox()
{
    std::deque<std::pair<ui::BrowserConnectionId, std::string>> pending;
    {
        const std::lock_guard lock(mutex_);
        pending.swap(outbox_);
    }
    for (auto& [connection, json] : pending) {
        for (auto& slot : slots_) {
            if (slot->connection != connection || !slot->introduced) {
                continue;
            }
            // A host that stops reading for longer than the timeout is treated as gone.
            if (!WriteFrame(slot->pipe.Get(), json, browser::kMaxOutboundBytes, stopEvent_, kWriteTimeoutMs)) {
                Close(*slot, true);
                Listen(*slot);
            }
            break;
        }
    }
}

void BrowserBridge::Run()
{
    for (auto& slot : slots_) {
        Listen(*slot);
    }
    std::vector<HANDLE> waits{stopEvent_, wakeEvent_};
    for (auto& slot : slots_) {
        waits.push_back(slot->event.Get());
    }
    for (;;) {
        const DWORD result = ::WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), FALSE, INFINITE);
        if (result == WAIT_OBJECT_0 || result == WAIT_FAILED) {
            break;
        }
        if (result == WAIT_OBJECT_0 + 1) {
            FlushOutbox();
            continue;
        }
        const size_t index = result - WAIT_OBJECT_0 - 2;
        if (index >= slots_.size()) {
            continue;
        }
        Slot& slot = *slots_[index];
        if (slot.phase == Slot::Phase::Dead) {
            ::ResetEvent(slot.event.Get());
            continue;
        }
        if (!Advance(slot)) {
            Close(slot, true);
            Listen(slot);
        }
    }
    for (auto& slot : slots_) {
        Close(*slot, false);
    }
}

}  // namespace ldm::platform
