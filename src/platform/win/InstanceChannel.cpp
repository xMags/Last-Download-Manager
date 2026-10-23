#include "platform/win/InstanceChannel.h"

#include "base/Utf.h"
#include "core/FileNames.h"
#include "core/Url.h"
#include "platform/win/PipeIo.h"

#include <nlohmann/json.hpp>

#include <filesystem>

namespace ldm::platform {

namespace {

constexpr size_t kMaxFieldBytes = 32 * 1024;

bool WriteMessage(HANDLE pipe, const std::string& payload, HANDLE stopEvent)
{
    return WriteFrame(pipe, payload, kMaxMessageBytes, stopEvent);
}

std::optional<std::string> ReadMessage(HANDLE pipe, HANDLE stopEvent)
{
    return ReadFrame(pipe, kMaxMessageBytes, stopEvent);
}

std::optional<std::string> OptionalString(const nlohmann::json& object, const char* key, bool& valid)
{
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) {
        return std::nullopt;
    }
    if (!it->is_string() || it->get_ref<const std::string&>().size() > kMaxFieldBytes) {
        valid = false;
        return std::nullopt;
    }
    return it->get<std::string>();
}

bool Flag(const nlohmann::json& object, const char* key, bool& valid)
{
    const auto it = object.find(key);
    if (it == object.end()) {
        return false;
    }
    if (!it->is_boolean()) {
        valid = false;
        return false;
    }
    return it->get<bool>();
}

}  // namespace

std::string EncodeLaunchRequest(const core::LaunchRequest& request, std::string_view requestId)
{
    nlohmann::json message = {
        {"protocol", kProtocolVersion},
        {"id", std::string(requestId)},
        {"type", "launch"},
        {"silent", request.silent},
        {"queueOnly", request.queueOnly},
        {"startQueue", request.startQueue},
        {"tray", request.tray},
    };
    if (request.url) {
        message["url"] = *request.url;
    }
    if (request.saveDir) {
        message["saveDir"] = *request.saveDir;
    }
    if (request.fileName) {
        message["fileName"] = *request.fileName;
    }
    return message.dump();
}

std::optional<core::LaunchRequest> DecodeLaunchRequest(std::string_view json)
{
    const nlohmann::json message = nlohmann::json::parse(json, nullptr, false);
    if (message.is_discarded() || !message.is_object()) {
        return std::nullopt;
    }
    const auto protocol = message.find("protocol");
    const auto type = message.find("type");
    const auto id = message.find("id");
    if (protocol == message.end() || !protocol->is_number_integer() || protocol->get<int>() != kProtocolVersion ||
        type == message.end() || !type->is_string() || type->get<std::string>() != "launch" || id == message.end() ||
        !id->is_string() || id->get_ref<const std::string&>().size() > 64) {
        return std::nullopt;
    }
    bool valid = true;
    core::LaunchRequest request;
    request.url = OptionalString(message, "url", valid);
    request.saveDir = OptionalString(message, "saveDir", valid);
    request.fileName = OptionalString(message, "fileName", valid);
    request.silent = Flag(message, "silent", valid);
    request.queueOnly = Flag(message, "queueOnly", valid);
    request.startQueue = Flag(message, "startQueue", valid);
    request.tray = Flag(message, "tray", valid);
    if (!valid) {
        return std::nullopt;
    }
    // The same rules as the command line: the sender is another process and is not trusted.
    if (request.url && !core::ParseUrl(*request.url)) {
        return std::nullopt;
    }
    if (request.saveDir && !std::filesystem::path(base::ToUtf16(*request.saveDir)).is_absolute()) {
        return std::nullopt;
    }
    if (request.fileName && (request.fileName->empty() || core::SanitizeFileName(*request.fileName) != *request.fileName)) {
        return std::nullopt;
    }
    return request;
}

std::unique_ptr<InstanceLock> InstanceLock::TryAcquire()
{
    const std::wstring name = L"Local\\LastDownloadManager.Instance-" + SessionAndUserSuffix();
    HANDLE mutex = ::CreateMutexW(nullptr, TRUE, name.c_str());
    if (mutex == nullptr) {
        return nullptr;
    }
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        ::CloseHandle(mutex);
        return nullptr;
    }
    return std::unique_ptr<InstanceLock>(new InstanceLock(mutex));  // private constructor
}

InstanceLock::~InstanceLock()
{
    if (mutex_ != nullptr) {
        ::ReleaseMutex(mutex_);
        ::CloseHandle(mutex_);
    }
}

InstanceServer::InstanceServer(std::function<void(core::LaunchRequest)> onRequest) : onRequest_(std::move(onRequest)) {}

InstanceServer::~InstanceServer()
{
    if (stopEvent_ != nullptr) {
        ::SetEvent(stopEvent_);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    if (firstPipe_ != INVALID_HANDLE_VALUE) {
        ::CloseHandle(firstPipe_);
    }
    if (stopEvent_ != nullptr) {
        ::CloseHandle(stopEvent_);
    }
}

bool InstanceServer::Start()
{
    stopEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    OwnerOnlySecurity security;
    if (stopEvent_ == nullptr || !security.Valid()) {
        return false;
    }
    // FILE_FLAG_FIRST_PIPE_INSTANCE fails if anyone created the name first, so another program
    // cannot pose as LDM and receive the commands.
    firstPipe_ = ::CreateNamedPipeW(InstancePipeName().c_str(),
                                    PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                    kMaxMessageBytes, kMaxMessageBytes, 0, security.Attributes());
    if (firstPipe_ == INVALID_HANDLE_VALUE) {
        return false;
    }
    thread_ = std::thread([this] { Run(); });
    return true;
}

void InstanceServer::Run()
{
    OwnedHandle connected(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    const HANDLE pipe = firstPipe_;
    while (true) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = connected.Get();
        ::ResetEvent(connected.Get());
        bool ready = false;
        if (::ConnectNamedPipe(pipe, &overlapped)) {
            ready = true;
        } else if (::GetLastError() == ERROR_PIPE_CONNECTED) {
            ready = true;
        } else if (::GetLastError() == ERROR_IO_PENDING) {
            const HANDLE waits[2] = {connected.Get(), stopEvent_};
            if (::WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) {
                ::CancelIoEx(pipe, &overlapped);
                DWORD ignored = 0;
                ::GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
                return;  // stop requested
            }
            DWORD ignored = 0;
            ready = ::GetOverlappedResult(pipe, &overlapped, &ignored, FALSE) != FALSE;
        }
        if (::WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) {
            return;
        }
        if (ready) {
            if (const auto payload = ReadMessage(pipe, stopEvent_)) {
                const nlohmann::json parsed = nlohmann::json::parse(*payload, nullptr, false);
                const std::string id = parsed.is_object() && parsed.contains("id") && parsed["id"].is_string()
                                           ? parsed["id"].get<std::string>()
                                           : std::string();
                const auto request = DecodeLaunchRequest(*payload);
                const nlohmann::json answer = {{"protocol", kProtocolVersion}, {"id", id}, {"ok", request.has_value()}};
                WriteMessage(pipe, answer.dump(), stopEvent_);
                if (request) {
                    onRequest_(*request);
                }
            }
        }
        ::FlushFileBuffers(pipe);
        ::DisconnectNamedPipe(pipe);
    }
}

bool ForwardToRunningInstance(const core::LaunchRequest& request)
{
    const std::wstring name = InstancePipeName();
    for (int attempt = 0; attempt < 10; ++attempt) {
        OwnedHandle pipe(::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
        if (!pipe.Valid()) {
            if (::GetLastError() != ERROR_PIPE_BUSY && ::GetLastError() != ERROR_FILE_NOT_FOUND) {
                return false;
            }
            // Busy with another launch, or still starting: wait a little and try again.
            ::WaitNamedPipeW(name.c_str(), 300);
            ::Sleep(100);
            continue;
        }
        const std::string id = std::to_string(::GetCurrentProcessId());
        if (!WriteMessage(pipe.Get(), EncodeLaunchRequest(request, id), nullptr)) {
            return false;
        }
        const auto answer = ReadMessage(pipe.Get(), nullptr);
        if (!answer) {
            return false;
        }
        const nlohmann::json parsed = nlohmann::json::parse(*answer, nullptr, false);
        return parsed.is_object() && parsed.value("ok", false) && parsed.value("id", std::string()) == id;
    }
    return false;
}

void AllowRunningInstanceToActivate()
{
    ::AllowSetForegroundWindow(ASFW_ANY);
}

}  // namespace ldm::platform
