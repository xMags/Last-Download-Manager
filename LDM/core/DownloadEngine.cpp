#include "DownloadEngine.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>


namespace Config {
constexpr long CONNECT_TIMEOUT_MS = 30000;
constexpr long RECEIVE_TIMEOUT_MS = 30000;
constexpr int64_t MIN_SIZE_FOR_MULTIPART = 1024 * 1024;
constexpr int64_t MIN_PART_SIZE = 512 * 1024;
constexpr int MAX_PARALLEL_SEGMENTS = 8;
constexpr int MAX_CHUNK_RETRIES = 3;
constexpr int MAX_DOWNLOAD_RETRIES = 5;  // Download-level auto-retry
constexpr int BASE_CHUNK_RETRY_MS = 500;
constexpr int BASE_DOWNLOAD_RETRY_MS = 2000;  // Longer delay between download retries
constexpr int64_t LARGE_BUFFER_THRESHOLD = 8 * 1024 * 1024;
constexpr int SPEED_UPDATE_INTERVAL_MS = 1000;  // Update speed every 1 second
} // namespace Config

// Helper to get WinINet error description
static std::string GetWinINetError(DWORD errorCode) {
  char buffer[512] = {0};
  DWORD len = sizeof(buffer);
  
  if (InternetGetLastResponseInfoA(&errorCode, buffer, &len) && len > 0) {
    // Truncate at first newline if present
    char* newline = strchr(buffer, '\n');
    if (newline) *newline = '\0';
    // Trim carriage return if present
    char* cr = strchr(buffer, '\r');
    if (cr) *cr = '\0';
    
    // Sometimes response info is empty even if function succeeds
    if (strlen(buffer) > 0) {
        return std::string(buffer);
    }
  }
  
  // Fallback to common error descriptions
  switch (errorCode) {
    case ERROR_INTERNET_TIMEOUT: return "Connection timed out";
    case ERROR_INTERNET_NAME_NOT_RESOLVED: return "Server name could not be resolved";
    case ERROR_INTERNET_CANNOT_CONNECT: return "Cannot connect to server";
    case ERROR_INTERNET_CONNECTION_ABORTED: return "Connection aborted";
    case ERROR_INTERNET_CONNECTION_RESET: return "Connection reset by server";
    case ERROR_INTERNET_DISCONNECTED: return "Internet disconnected";
    case ERROR_INTERNET_SERVER_UNREACHABLE: return "Server unreachable";
    case ERROR_INTERNET_OPERATION_CANCELLED: return "Operation cancelled";
    case ERROR_INTERNET_INVALID_URL: return "Invalid URL";
    case ERROR_INTERNET_SEC_CERT_DATE_INVALID: return "SSL certificate date invalid";
    case ERROR_INTERNET_SEC_CERT_CN_INVALID: return "SSL certificate name mismatch";
    case ERROR_INTERNET_HTTP_TO_HTTPS_ON_REDIR: return "Redirected HTTP to HTTPS";
    case ERROR_INTERNET_HTTPS_TO_HTTP_ON_REDIR: return "Redirected HTTPS to HTTP";
    case ERROR_INTERNET_INCORRECT_HANDLE_TYPE: return "Incorrect handle type";
    case ERROR_INTERNET_LOGIN_FAILURE: return "Login failure";
    case ERROR_INTERNET_INVALID_OPERATION: return "Invalid operation";
    // case ERROR_INTERNET_OPERATION_IN_PROGRESS: return "Operation in progress"; // Not defined in all SDKs
    case ERROR_INTERNET_INCORRECT_PASSWORD: return "Incorrect password";
    case ERROR_HTTP_HEADER_NOT_FOUND: return "HTTP header not found";
    case ERROR_HTTP_DOWNLEVEL_SERVER: return "HTTP downlevel server";
    case ERROR_HTTP_INVALID_SERVER_RESPONSE: return "Invalid server response";
    case ERROR_HTTP_INVALID_HEADER: return "Invalid HTTP header";
    case ERROR_HTTP_INVALID_QUERY_REQUEST: return "Invalid HTTP query request";
    case ERROR_HTTP_HEADER_ALREADY_EXISTS: return "HTTP header already exists";
    case ERROR_HTTP_REDIRECT_FAILED: return "HTTP redirect failed";
    case ERROR_HTTP_NOT_REDIRECTED: return "HTTP not redirected";
    case ERROR_HTTP_COOKIE_NEEDS_CONFIRMATION: return "Cookie needs confirmation";
    case ERROR_HTTP_COOKIE_DECLINED: return "Cookie declined";
    case ERROR_HTTP_REDIRECT_NEEDS_CONFIRMATION: return "Redirect needs confirmation";
    default: return "Error code: " + std::to_string(errorCode);
  }
}

void DownloadEngine::ConfigureSessionTimeouts(HINTERNET session) {
  if (!session) {
    return;
  }

  DWORD timeout = Config::CONNECT_TIMEOUT_MS;
  InternetSetOption(session, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout,
                    sizeof(DWORD));
  timeout = Config::RECEIVE_TIMEOUT_MS;
  InternetSetOption(session, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout,
                    sizeof(DWORD));
}

HINTERNET DownloadEngine::OpenSession(const std::string &userAgent,
                                      const std::string &proxyUrl) {
  return InternetOpenA(
      userAgent.c_str(),
      proxyUrl.empty() ? INTERNET_OPEN_TYPE_PRECONFIG : INTERNET_OPEN_TYPE_PROXY,
      proxyUrl.empty() ? NULL : proxyUrl.c_str(), NULL, 0);
}

void DownloadEngine::CloseSessionHandle(
    const std::shared_ptr<SessionEntry> &entry) {
  if (!entry) {
    return;
  }

  std::lock_guard<std::mutex> lock(entry->handleMutex);
  if (entry->handle) {
    InternetCloseHandle(entry->handle);
    entry->handle = nullptr;
  }
}

void DownloadEngine::CloseSessionIfIdle(
    const std::shared_ptr<SessionEntry> &entry) {
  if (!entry) {
    return;
  }

  if (entry->closing.load() && entry->activeCount.load() == 0) {
    CloseSessionHandle(entry);
  }
}

DownloadEngine::SessionUsage::SessionUsage(
    std::shared_ptr<SessionEntry> entry)
    : m_entry(std::move(entry)) {
  if (m_entry) {
    m_entry->activeCount.fetch_add(1);
  }
}

DownloadEngine::SessionUsage::~SessionUsage() {
  if (!m_entry) {
    return;
  }

  int remaining = m_entry->activeCount.fetch_sub(1) - 1;
  if (remaining == 0 && m_entry->closing.load()) {
    CloseSessionHandle(m_entry);
  }
}

HINTERNET DownloadEngine::SessionUsage::handle() const {
  if (!m_entry) return nullptr;
  std::lock_guard<std::mutex> lock(m_entry->handleMutex);
  return m_entry->handle;
}

bool DownloadEngine::ParseContentRangeStart(const std::string &value,
                                            int64_t &startOut) {
  size_t spacePos = value.find(' ');
  if (spacePos == std::string::npos) {
    return false;
  }

  size_t startPos = spacePos + 1;
  while (startPos < value.size() &&
         std::isspace(static_cast<unsigned char>(value[startPos]))) {
    startPos++;
  }

  size_t dashPos = value.find('-', startPos);
  if (dashPos == std::string::npos || dashPos == startPos) {
    return false;
  }

  std::string startStr = value.substr(startPos, dashPos - startPos);
  char *endPtr = nullptr;
  int64_t parsed = _strtoi64(startStr.c_str(), &endPtr, 10);
  if (!endPtr || endPtr == startStr.c_str() || *endPtr != '\0') {
    return false;
  }

  startOut = parsed;
  return true;
}

DownloadEngine::DownloadEngine()
    : m_maxConnections(8), m_useNativeCAStore(true),
      m_state(std::make_shared<EngineState>()) {
  m_state->userAgent = "LastDownloadManager/1.0";
  m_state->proxyUrl.clear();
  m_state->verifySSL.store(true);
  m_state->speedLimitBytes.store(0);

  auto entry = std::make_shared<SessionEntry>();
  entry->handle = OpenSession(m_state->userAgent, m_state->proxyUrl);
  if (entry->handle) {
    ConfigureSessionTimeouts(entry->handle);
  }

  m_state->session = entry;
  m_state->running.store(entry->handle != nullptr);
}

DownloadEngine::~DownloadEngine() {
  if (m_state) {
    m_state->running.store(false);

    std::shared_ptr<SessionEntry> currentSession;
    std::vector<std::shared_ptr<SessionEntry>> retiredSessions;
    {
      std::lock_guard<std::mutex> lock(m_state->sessionMutex);
      currentSession = m_state->session;
      retiredSessions = m_state->retiredSessions;
    }

    if (currentSession) {
      currentSession->closing.store(true);
      CloseSessionIfIdle(currentSession);
    }

    for (const auto &entry : retiredSessions) {
      if (!entry) {
        continue;
      }
      entry->closing.store(true);
      CloseSessionIfIdle(entry);
    }
  }

  std::vector<std::future<bool>> activeDownloads;
  {
    std::lock_guard<std::mutex> lock(m_activeDownloadsMutex);
    activeDownloads.swap(m_activeDownloads);
  }

  for (auto &future : activeDownloads) {
    if (future.valid()) {
      future.wait();
    }
  }

  if (m_workerThread.joinable()) {
    m_workerThread.join();
  }

  if (m_state) {
    std::lock_guard<std::mutex> lock(m_state->sessionMutex);
    for (const auto &entry : m_state->retiredSessions) {
      CloseSessionIfIdle(entry);
    }
    CloseSessionIfIdle(m_state->session);
  }
}

void DownloadEngine::SetProgressCallback(ProgressCallback callback) {
  if (!m_state) {
    return;
  }

  std::lock_guard<std::mutex> lock(m_state->callbackMutex);
  m_state->progressCallback = callback;
}

void DownloadEngine::SetCompletionCallback(CompletionCallback callback) {
  if (!m_state) {
    return;
  }

  std::lock_guard<std::mutex> lock(m_state->callbackMutex);
  m_state->completionCallback = callback;
}

void DownloadEngine::SetSpeedLimit(int64_t bytesPerSecond) {
  if (!m_state) {
    return;
  }

  m_state->speedLimitBytes.store(bytesPerSecond);
}

void DownloadEngine::SetUserAgent(const std::string &userAgent) {
  if (!m_state) {
    return;
  }

  std::lock_guard<std::mutex> lock(m_state->sessionMutex);
  m_state->userAgent = userAgent;
}

void DownloadEngine::SetSSLVerification(bool verify) {
  if (!m_state) {
    return;
  }

  m_state->verifySSL.store(verify);
}

bool DownloadEngine::GetSSLVerification() const {
  if (!m_state) {
    return true;
  }

  return m_state->verifySSL.load();
}

void DownloadEngine::CleanupRetiredSessions(
    const std::shared_ptr<EngineState> &state) {
  if (!state) {
    return;
  }

  std::lock_guard<std::mutex> lock(state->sessionMutex);
  auto &retired = state->retiredSessions;
  retired.erase(
      std::remove_if(retired.begin(), retired.end(),
                     [](const std::shared_ptr<SessionEntry> &entry) {
                       if (!entry) {
                         return true;
                       }
                       if (entry->closing.load() &&
                           entry->activeCount.load() == 0) {
                         CloseSessionHandle(entry);
                       }
                       return entry->activeCount.load() == 0 &&
                              entry->handle == nullptr;
                     }),
      retired.end());
}

bool DownloadEngine::GetFileInfo(const std::string &url, int64_t &fileSize,
                                 bool &resumable) {
  if (!m_state || !m_state->running.load())
    return false;

  std::shared_ptr<SessionEntry> sessionEntry;
  {
    std::lock_guard<std::mutex> lock(m_state->sessionMutex);
    sessionEntry = m_state->session;
  }

  if (!sessionEntry || !sessionEntry->handle)
    return false;

  SessionUsage sessionUsage(sessionEntry);
  HINTERNET hSession = sessionUsage.handle();
  if (!hSession)
    return false;

  DWORD flags = INTERNET_FLAG_NO_UI | INTERNET_FLAG_RELOAD;
  if (!m_state->verifySSL.load()) {
    flags |= (INTERNET_FLAG_IGNORE_CERT_CN_INVALID |
              INTERNET_FLAG_IGNORE_CERT_DATE_INVALID);
  }

  HINTERNET hFile =
      InternetOpenUrlA(hSession, url.c_str(), "Head: Trigger", -1, flags, 0);

  if (!hFile)
    return false;

  // Content Length
  DWORD size = 0;
  DWORD bufferSize = sizeof(size);
  // Try to get 64-bit size first (Content-Length)
  char clBuffer[64] = {0};
  bufferSize = sizeof(clBuffer);

  if (HttpQueryInfoA(hFile, HTTP_QUERY_CONTENT_LENGTH, clBuffer, &bufferSize,
                     NULL)) {
    fileSize = _strtoi64(clBuffer, NULL, 10);
  } else {
    fileSize = -1;
  }

  DWORD statusCode = 0;
  DWORD statusSize = sizeof(statusCode);
  if (HttpQueryInfoA(hFile, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                     &statusCode, &statusSize, NULL) &&
      statusCode == 416) {
    InternetCloseHandle(hFile);
    return false;
  }

  // Accept-Ranges
  char rangesBuffer[64] = {0};
  bufferSize = sizeof(rangesBuffer);
  if (HttpQueryInfoA(hFile, HTTP_QUERY_ACCEPT_RANGES, rangesBuffer, &bufferSize,
                     NULL)) {
    std::string ranges(rangesBuffer);
    resumable = (ranges.find("bytes") != std::string::npos);
  } else {
    resumable = false;
  }

  InternetCloseHandle(hFile);
  return true;
}

int64_t DownloadEngine::GetExistingFileSize(const std::string &filePath) {
  std::ifstream checkFile(filePath, std::ios::binary | std::ios::ate);
  if (!checkFile.is_open()) {
    return 0;
  }
  return checkFile.tellg();
}

bool DownloadEngine::PreallocateFile(const std::string &filePath,
                                     int64_t size) {
  if (size <= 0) {
    return false;
  }

  HANDLE fileHandle =
      CreateFileA(filePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL,
                  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (fileHandle == INVALID_HANDLE_VALUE) {
    return false;
  }

  LARGE_INTEGER distance;
  distance.QuadPart = size;
  if (!SetFilePointerEx(fileHandle, distance, NULL, FILE_BEGIN)) {
    CloseHandle(fileHandle);
    return false;
  }
  if (!SetEndOfFile(fileHandle)) {
    CloseHandle(fileHandle);
    return false;
  }

  CloseHandle(fileHandle);
  return true;
}

bool DownloadEngine::StartDownload(std::shared_ptr<Download> download) {
  if (!download)
    return false;

  auto state = m_state;
  if (!state || !state->running.load())
    return false;

  download->ResetSpeed();
  download->SetStatus(DownloadStatus::Downloading);
  download->UpdateLastTryTime();

  CleanupCompletedDownloads();

  {
    std::lock_guard<std::mutex> lock(m_activeDownloadsMutex);
    m_activeDownloads.push_back(
        std::async(std::launch::async, [this, state, download]() {
          int64_t fileSize = -1;
          bool resumable = false;

          if (GetFileInfo(download->GetUrl(), fileSize, resumable)) {
            download->SetTotalSize(fileSize);
          }

          int connections = std::max(1, m_maxConnections);
          if (connections > Config::MAX_PARALLEL_SEGMENTS) {
            connections = Config::MAX_PARALLEL_SEGMENTS;
          }

          if (fileSize > 0 && fileSize < Config::MIN_SIZE_FOR_MULTIPART) {
            connections = 1;
          } else if (fileSize > 0) {
            int64_t maxBySize = fileSize / Config::MIN_PART_SIZE;
            if (maxBySize < 1) {
              maxBySize = 1;
            }
            if (connections > static_cast<int>(maxBySize)) {
              connections = static_cast<int>(maxBySize);
            }
          }

          bool useMultiSegment = resumable && fileSize > 0 && connections > 1;
          
          // Check if we have existing chunks (for resume)
          auto existingChunks = download->GetChunksCopy();
          bool hasExistingChunks = !existingChunks.empty() && 
                                   existingChunks[0].startByte == 0;
          
          // Only reinitialize chunks if:
          // 1. No existing chunks, OR
          // 2. Chunk count matches desired connections for multi-segment, OR
          // 3. Switching from multi to single or vice versa
          if (!hasExistingChunks || 
              (useMultiSegment && existingChunks.size() != static_cast<size_t>(connections)) ||
              (!useMultiSegment && existingChunks.size() != 1)) {
            download->InitializeChunks(useMultiSegment ? connections : 1);
          }

          if (useMultiSegment) {
            return PerformMultiSegmentDownload(state, download, connections);
          }
          return PerformDownload(state, download);
        }));
  }

  return true;
}

void DownloadEngine::PauseDownload(std::shared_ptr<Download> download) {
  if (download) {
    download->SetStatus(DownloadStatus::Paused);
    // Close all tracked handles for this download atomically
    if (m_state) {
      std::lock_guard<std::mutex> lock(m_state->requestHandlesMutex);
      auto it = m_state->requestHandles.find(download->GetId());
      if (it != m_state->requestHandles.end()) {
        for (HINTERNET h : it->second) {
          if (h) InternetCloseHandle(h);
        }
        m_state->requestHandles.erase(it);
      }
    }
  }
}


void DownloadEngine::ResumeDownload(std::shared_ptr<Download> download) {
  if (!download)
    return;
  if (download->GetStatus() == DownloadStatus::Completed)
    return;
  if (download->GetStatus() == DownloadStatus::Paused ||
      download->GetStatus() == DownloadStatus::Error) {
    // Reset error state and retry count before resuming
    download->ResetRetry();
    download->SetErrorMessage("");
    StartDownload(download);
  }
}

void DownloadEngine::CancelDownload(std::shared_ptr<Download> download) {
  if (download) {
    download->SetStatus(DownloadStatus::Cancelled);
    // Close all tracked handles for this download atomically
    if (m_state) {
      std::lock_guard<std::mutex> lock(m_state->requestHandlesMutex);
      auto it = m_state->requestHandles.find(download->GetId());
      if (it != m_state->requestHandles.end()) {
        for (HINTERNET h : it->second) {
          if (h) InternetCloseHandle(h);
        }
        m_state->requestHandles.erase(it);
      }
    }
  }
}

void DownloadEngine::SetProxy(const std::string &proxyHost, int proxyPort) {
  if (!m_state) {
    return;
  }

  std::string newProxyUrl;
  if (!proxyHost.empty()) {
    if (proxyPort <= 0 || proxyPort > 65535) {
      std::cerr << "Invalid proxy port: " << proxyPort
                << " (must be 1-65535)" << std::endl;
      return;  // Don't change proxy settings on invalid input
    }
    
    bool validHost = true;
    for (char c : proxyHost) {
      if (std::isspace(static_cast<unsigned char>(c))) {
        validHost = false;
        break;
      }
    }

    if (!validHost) {
      std::cerr << "Invalid proxy host format: " << proxyHost << std::endl;
      return;  // Don't change proxy settings on invalid input
    }
    
    newProxyUrl = proxyHost + ":" + std::to_string(proxyPort);
  }

  if (!ReinitializeSession(newProxyUrl)) {
    std::cerr << "Failed to apply proxy settings" << std::endl;
  }
}

bool DownloadEngine::ReinitializeSession(const std::string &proxyUrl) {
  if (!m_state) {
    return false;
  }

  std::string userAgent;
  {
    std::lock_guard<std::mutex> lock(m_state->sessionMutex);
    userAgent = m_state->userAgent;
  }

  HINTERNET newSession = OpenSession(userAgent, proxyUrl);
  if (!newSession) {
    return false;
  }

  ConfigureSessionTimeouts(newSession);

  auto newEntry = std::make_shared<SessionEntry>();
  newEntry->handle = newSession;

  std::shared_ptr<SessionEntry> oldEntry;
  {
    std::lock_guard<std::mutex> lock(m_state->sessionMutex);
    oldEntry = m_state->session;
    m_state->session = newEntry;
    m_state->proxyUrl = proxyUrl;
    if (oldEntry) {
      oldEntry->closing.store(true);
      m_state->retiredSessions.push_back(oldEntry);
    }
  }

  if (oldEntry) {
    CloseSessionIfIdle(oldEntry);
  }
  CleanupRetiredSessions(m_state);

  m_state->running.store(true);
  return true;
}

bool DownloadEngine::PerformDownload(std::shared_ptr<EngineState> state,
                                     std::shared_ptr<Download> download) {
  if (!state || !download || !state->running.load())
    return false;

  std::shared_ptr<SessionEntry> sessionEntry;
  {
    std::lock_guard<std::mutex> lock(state->sessionMutex);
    sessionEntry = state->session;
  }
  if (!sessionEntry || !sessionEntry->handle)
    return false;

  SessionUsage sessionUsage(sessionEntry);
  HINTERNET hSession = sessionUsage.handle();
  if (!hSession)
    return false;

  ProgressCallback progressCallback;
  CompletionCallback completionCallback;
  {
    std::lock_guard<std::mutex> lock(state->callbackMutex);
    progressCallback = state->progressCallback;
    completionCallback = state->completionCallback;
  }

  std::string url = download->GetUrl();
  std::string savePath = download->GetSavePath();
  CreateDirectoryA(savePath.c_str(), NULL);
  std::string filePath = savePath + "\\" + download->GetFilename();

  // Check existing size for resume
  int64_t existingSize = GetExistingFileSize(filePath);

  bool shouldResume = (existingSize > 0 && download->GetDownloadedSize() > 0 &&
                       download->GetStatus() == DownloadStatus::Downloading);

  // Headers for resume
  std::string headers = "";
  if (shouldResume) {
    download->SetDownloadedSize(existingSize);
    headers = "Range: bytes=" + std::to_string(existingSize) + "-";
  } else {
    existingSize = 0;
    download->SetDownloadedSize(0);
  }

  // Open Request
  DWORD flags = INTERNET_FLAG_NO_UI | INTERNET_FLAG_RELOAD |
                INTERNET_FLAG_KEEP_CONNECTION;
  if (!state->verifySSL.load())
    flags |= (INTERNET_FLAG_IGNORE_CERT_CN_INVALID |
              INTERNET_FLAG_IGNORE_CERT_DATE_INVALID);

  HINTERNET hUrl = InternetOpenUrlA(
      hSession, url.c_str(), headers.empty() ? NULL : headers.c_str(),
      headers.empty() ? -1 : static_cast<DWORD>(headers.length()), flags, 0);

  if (!hUrl) {
    DWORD err = GetLastError();
    std::cerr << "[Download] Connection failed: " << GetWinINetError(err) << std::endl;
    
    // Auto-retry on connection failure
    int retryCount = download->GetRetryCount();
    if (retryCount < Config::MAX_DOWNLOAD_RETRIES) {
      std::cerr << "[Download] Auto-retry " << (retryCount + 1) 
                << "/" << Config::MAX_DOWNLOAD_RETRIES << std::endl;
      download->IncrementRetry();
      std::this_thread::sleep_for(
          std::chrono::milliseconds(Config::BASE_DOWNLOAD_RETRY_MS * (1 << std::min(retryCount, 4))));
      if (download->GetStatus() == DownloadStatus::Cancelled ||
          download->GetStatus() == DownloadStatus::Paused)
        return false;
      download->SetStatus(DownloadStatus::Downloading);
      return PerformDownload(state, download);
    }

    download->SetStatus(DownloadStatus::Error);
    download->SetErrorMessage("Connection failed: " + GetWinINetError(err));
    if (completionCallback)
      completionCallback(download->GetId(), false, "Connection failed");
    return false;
  }

  TrackRequestHandle(state, download->GetId(), hUrl);

  if (shouldResume) {
    bool resumeValid = false;
    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (HttpQueryInfoA(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                       &statusCode, &statusSize, NULL)) {
      if (statusCode == 206) {
        char rangeBuffer[128] = {0};
        DWORD rangeSize = sizeof(rangeBuffer);
        if (HttpQueryInfoA(hUrl, HTTP_QUERY_CONTENT_RANGE, rangeBuffer,
                           &rangeSize, NULL)) {
          int64_t rangeStart = 0;
          resumeValid = ParseContentRangeStart(rangeBuffer, rangeStart) &&
                        rangeStart == existingSize;
        }
      }
    }

    if (!resumeValid) {
      HINTERNET oldHandle = hUrl;
      UntrackRequestHandle(state, download->GetId(), oldHandle);
      InternetCloseHandle(oldHandle);
      shouldResume = false;
      existingSize = 0;
      download->SetDownloadedSize(0);
      headers.clear();

      hUrl = InternetOpenUrlA(hSession, url.c_str(), NULL, -1, flags, 0);
      if (!hUrl) {
        DWORD err = GetLastError();
        std::cerr << "[Download] Resume restart failed: " << GetWinINetError(err) << std::endl;
        
        // Auto-retry on restart failure
        int retryCount = download->GetRetryCount();
        if (retryCount < Config::MAX_DOWNLOAD_RETRIES) {
          std::cerr << "[Download] Auto-retry " << (retryCount + 1) 
                    << "/" << Config::MAX_DOWNLOAD_RETRIES << std::endl;
          download->IncrementRetry();
          std::this_thread::sleep_for(
              std::chrono::milliseconds(Config::BASE_DOWNLOAD_RETRY_MS * (1 << std::min(retryCount, 4))));
          if (download->GetStatus() == DownloadStatus::Cancelled ||
              download->GetStatus() == DownloadStatus::Paused)
            return false;
          return PerformDownload(state, download);
        }
        
        download->SetStatus(DownloadStatus::Error);
        download->SetErrorMessage("Failed to restart download: " + GetWinINetError(err));
        if (completionCallback)
          completionCallback(download->GetId(), false, "Connection failed");
        return false;
      }
      TrackRequestHandle(state, download->GetId(), hUrl);
    }
  }

  // Open output file
  std::ofstream file;
  if (shouldResume) {
    file.open(filePath, std::ios::binary | std::ios::app);
  } else {
    file.open(filePath, std::ios::binary | std::ios::trunc);
  }

  if (!file.is_open()) {
    UntrackRequestHandle(state, download->GetId(), hUrl);
    InternetCloseHandle(hUrl);
    download->SetStatus(DownloadStatus::Error);
    download->SetErrorMessage("File I/O Error");
    return false;
  }

  // Read Loop
  DWORD bytesRead = 0;
  char buffer[65536]; // 64KB buffer
  auto lastSpeedUpdate = std::chrono::steady_clock::now();
  auto lastThrottleUpdate = lastSpeedUpdate;
  int64_t lastBytes = shouldResume ? existingSize : 0;

  do {
    // Check Status
    if (!state->running.load() ||
        download->GetStatus() == DownloadStatus::Cancelled ||
        download->GetStatus() == DownloadStatus::Paused) {
      file.close();
      UntrackRequestHandle(state, download->GetId(), hUrl);
      InternetCloseHandle(hUrl);
      if (state->running.load() && completionCallback)
        completionCallback(download->GetId(), false, "User Aborted");
      return false;
    }

    if (InternetReadFile(hUrl, buffer, sizeof(buffer), &bytesRead)) {
      if (bytesRead > 0) {
        file.write(buffer, bytesRead);
        if (file.fail()) {
          file.close();
          UntrackRequestHandle(state, download->GetId(), hUrl);
          InternetCloseHandle(hUrl);
          download->SetStatus(DownloadStatus::Error);
          download->SetErrorMessage(
              "Disk write failed - check available disk space");
          if (completionCallback)
            completionCallback(download->GetId(), false, "File I/O Error");
          return false;
        }

        int64_t currentSize = download->GetDownloadedSize() + bytesRead;
        download->SetDownloadedSize(currentSize);

        // Speed Update
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - lastSpeedUpdate)
                           .count();
        if (elapsed >= Config::SPEED_UPDATE_INTERVAL_MS) {
          double speed = 0.0;
          if (elapsed > 0) {
            speed = static_cast<double>(currentSize - lastBytes) / (elapsed / 1000.0);
          }
          if (speed >= 0.0) {
            download->SetSpeed(speed);
          }
          lastSpeedUpdate = now;
          lastBytes = currentSize;

          if (progressCallback) {
            progressCallback(download->GetId(), currentSize,
                             download->GetTotalSize(), speed);
          }
        }

        // Simple speed limit (blocking)
        int64_t speedLimit = state->speedLimitBytes.load();
        if (speedLimit > 0) {
          auto now = std::chrono::steady_clock::now();
          auto elapsedMs =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  now - lastThrottleUpdate)
                  .count();
          double targetMs = (bytesRead * 1000.0) / speedLimit;
          if (elapsedMs < targetMs) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(static_cast<int>(targetMs - elapsedMs)));
          }
          lastThrottleUpdate = std::chrono::steady_clock::now();
        }
      }
    } else {
      // Read Error - attempt retry
      DWORD err = GetLastError();
      std::cerr << "[Download] Read failed: " << GetWinINetError(err) << std::endl;
      file.close();
      UntrackRequestHandle(state, download->GetId(), hUrl);
      InternetCloseHandle(hUrl);
      
      // Auto-retry on read failure
      int retryCount = download->GetRetryCount();
      if (retryCount < Config::MAX_DOWNLOAD_RETRIES) {
        std::cerr << "[Download] Auto-retry " << (retryCount + 1) 
                  << "/" << Config::MAX_DOWNLOAD_RETRIES << std::endl;
        download->IncrementRetry();
        std::this_thread::sleep_for(
            std::chrono::milliseconds(Config::BASE_DOWNLOAD_RETRY_MS * (1 << std::min(retryCount, 4))));
        if (download->GetStatus() == DownloadStatus::Cancelled ||
            download->GetStatus() == DownloadStatus::Paused)
          return false;
        download->SetStatus(DownloadStatus::Downloading);
        return PerformDownload(state, download);
      }
      
      download->SetStatus(DownloadStatus::Error);
      download->SetErrorMessage("Read failed: " + GetWinINetError(err));
      if (completionCallback)
        completionCallback(download->GetId(), false, "Read Error");
      return false;
    }

  } while (bytesRead > 0);

  file.close();
  UntrackRequestHandle(state, download->GetId(), hUrl);
  InternetCloseHandle(hUrl);

  download->SetStatus(DownloadStatus::Completed);
  download->ResetRetry();
  if (completionCallback)
    completionCallback(download->GetId(), true, "");

  return true;
}

DownloadEngine::ChunkResult DownloadEngine::PerformChunkDownload(
    std::shared_ptr<EngineState> state, std::shared_ptr<Download> download,
    int chunkIndex, int64_t rangeStart, int64_t rangeEnd,
    const std::string &partPath, int64_t speedLimitBytes, int64_t fileOffset) {
  if (!state || !download || !state->running.load())
    return ChunkResult::Failed;

  std::shared_ptr<SessionEntry> sessionEntry;
  {
    std::lock_guard<std::mutex> lock(state->sessionMutex);
    sessionEntry = state->session;
  }
  if (!sessionEntry || !sessionEntry->handle)
    return ChunkResult::Failed;

  SessionUsage sessionUsage(sessionEntry);
  HINTERNET hSession = sessionUsage.handle();
  if (!hSession)
    return ChunkResult::Failed;

  ProgressCallback progressCallback;
  {
    std::lock_guard<std::mutex> lock(state->callbackMutex);
    progressCallback = state->progressCallback;
  }

  std::string url = download->GetUrl();
  std::string headers = "Range: bytes=" + std::to_string(rangeStart) + "-" +
                        std::to_string(rangeEnd);

  DWORD flags = INTERNET_FLAG_NO_UI | INTERNET_FLAG_RELOAD |
                INTERNET_FLAG_KEEP_CONNECTION;
  if (!state->verifySSL.load())
    flags |= (INTERNET_FLAG_IGNORE_CERT_CN_INVALID |
              INTERNET_FLAG_IGNORE_CERT_DATE_INVALID);

  HINTERNET hUrl = InternetOpenUrlA(
      hSession, url.c_str(), headers.c_str(),
      static_cast<DWORD>(headers.length()), flags, 0);

  if (!hUrl) {
    DWORD err = GetLastError();
    std::cerr << "[Chunk " << chunkIndex << "] Connection failed: " 
              << GetWinINetError(err) << std::endl;
    return ChunkResult::NetworkError;
  }

  TrackRequestHandle(state, download->GetId(), hUrl);

  DWORD statusCode = 0;
  DWORD statusSize = sizeof(statusCode);
  if (!HttpQueryInfoA(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                      &statusCode, &statusSize, NULL)) {
    DWORD err = GetLastError();
    std::cerr << "[Chunk " << chunkIndex << "] Failed to get HTTP status: " 
              << GetWinINetError(err) << std::endl;
    UntrackRequestHandle(state, download->GetId(), hUrl);
    InternetCloseHandle(hUrl);
    return ChunkResult::NetworkError;
  }

  if (statusCode == 429 || statusCode == 503) {
    UntrackRequestHandle(state, download->GetId(), hUrl);
    InternetCloseHandle(hUrl);
    return ChunkResult::Throttled;
  }

  if (statusCode == 416) {
    UntrackRequestHandle(state, download->GetId(), hUrl);
    InternetCloseHandle(hUrl);
    return ChunkResult::RangeUnsupported;
  }

  if (statusCode != 206) {
    UntrackRequestHandle(state, download->GetId(), hUrl);
    InternetCloseHandle(hUrl);
    return ChunkResult::RangeUnsupported;
  }

  // Validate Content-Range header matches our request
  char contentRange[256] = {0};
  DWORD contentRangeSize = sizeof(contentRange);
  if (HttpQueryInfoA(hUrl, HTTP_QUERY_CONTENT_RANGE, contentRange,
                     &contentRangeSize, NULL)) {
    // Parse "bytes START-END/TOTAL" format
    int64_t serverStart = -1, serverEnd = -1;
    if (sscanf_s(contentRange, "bytes %lld-%lld", &serverStart, &serverEnd) >= 2) {
      if (serverStart != rangeStart) {
        // Server returned different range than requested - data would be corrupted
        UntrackRequestHandle(state, download->GetId(), hUrl);
        InternetCloseHandle(hUrl);
        return ChunkResult::Failed;
      }
    }
  }

  std::ofstream file(partPath, std::ios::binary | std::ios::in | std::ios::out);
  bool truncated = false;
  if (!file.is_open()) {
    file.clear();
    file.open(partPath, std::ios::binary | std::ios::trunc | std::ios::out);
    truncated = true;
  }
  if (!file.is_open()) {
    UntrackRequestHandle(state, download->GetId(), hUrl);
    InternetCloseHandle(hUrl);
    return ChunkResult::Failed;
  }

  // If we truncated the file but expected to resume at an offset, we'd corrupt data.
  // Fail this chunk so the caller can retry properly.
  if (truncated && fileOffset > 0) {
    file.close();
    UntrackRequestHandle(state, download->GetId(), hUrl);
    InternetCloseHandle(hUrl);
    return ChunkResult::Failed;
  }

  if (fileOffset > 0) {
    file.seekp(fileOffset, std::ios::beg);
    if (file.fail()) {
      file.close();
      UntrackRequestHandle(state, download->GetId(), hUrl);
      InternetCloseHandle(hUrl);
      return ChunkResult::Failed;
    }
  }

  DWORD bytesRead = 0;
  std::vector<char> buffer;
  int64_t rangeLength = (rangeEnd - rangeStart) + 1;
  if (rangeLength >= Config::LARGE_BUFFER_THRESHOLD) {
    buffer.resize(256 * 1024);
  } else {
    buffer.resize(64 * 1024);
  }
  auto lastProgressUpdate = std::chrono::steady_clock::now();
  auto lastThrottleUpdate = lastProgressUpdate;
  int64_t totalBytes = 0;

  do {
    if (!state->running.load() ||
        download->GetStatus() == DownloadStatus::Cancelled ||
        download->GetStatus() == DownloadStatus::Paused) {
      file.close();
      UntrackRequestHandle(state, download->GetId(), hUrl);
      InternetCloseHandle(hUrl);
      return ChunkResult::Aborted;
    }

    if (InternetReadFile(hUrl, buffer.data(),
                         static_cast<DWORD>(buffer.size()), &bytesRead)) {
      if (bytesRead > 0) {
        file.write(buffer.data(), bytesRead);
        if (file.fail()) {
          file.close();
          UntrackRequestHandle(state, download->GetId(), hUrl);
          InternetCloseHandle(hUrl);
          return ChunkResult::Failed;
        }

        totalBytes += bytesRead;
        download->UpdateChunkProgress(chunkIndex, rangeStart + totalBytes);

        // Notify progress periodically (speed calculated in main thread)
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - lastProgressUpdate)
                           .count();
        if (elapsed >= Config::SPEED_UPDATE_INTERVAL_MS && progressCallback) {
          progressCallback(download->GetId(), download->GetDownloadedSize(),
                           download->GetTotalSize(), 0.0);
          lastProgressUpdate = now;
        }

        if (speedLimitBytes > 0) {
          auto now = std::chrono::steady_clock::now();
          auto elapsedMs =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  now - lastThrottleUpdate)
                  .count();
          double targetMs = (bytesRead * 1000.0) / speedLimitBytes;
          if (elapsedMs < targetMs) {
            std::this_thread::sleep_for(std::chrono::milliseconds(
                static_cast<int>(targetMs - elapsedMs)));
          }
          lastThrottleUpdate = std::chrono::steady_clock::now();
        }
      }
    } else {
      DWORD err = GetLastError();
      std::cerr << "[Chunk " << chunkIndex << "] Read failed after " 
                << totalBytes << " bytes: " << GetWinINetError(err) << std::endl;
      file.close();
      UntrackRequestHandle(state, download->GetId(), hUrl);
      InternetCloseHandle(hUrl);
      return ChunkResult::NetworkError;
    }

  } while (bytesRead > 0);

  file.close();
  UntrackRequestHandle(state, download->GetId(), hUrl);
  InternetCloseHandle(hUrl);

  // Verify chunk received all expected bytes
  if (totalBytes != rangeLength) {
    std::cerr << "[Chunk " << chunkIndex << "] Incomplete: got " << totalBytes 
              << " of " << rangeLength << " bytes" << std::endl;
    return ChunkResult::NetworkError;
  }

  return ChunkResult::Success;
}

void DownloadEngine::TrackRequestHandle(const std::shared_ptr<EngineState> &state,
                                        int downloadId,
                                        HINTERNET requestHandle) {
  if (!state || !requestHandle) {
    return;
  }
  std::lock_guard<std::mutex> lock(state->requestHandlesMutex);
  state->requestHandles[downloadId].push_back(requestHandle);
}

void DownloadEngine::UntrackRequestHandle(const std::shared_ptr<EngineState> &state,
                                          int downloadId,
                                          HINTERNET requestHandle) {
  if (!state || !requestHandle) {
    return;
  }
  // Remove the specific handle from the vector
  std::lock_guard<std::mutex> lock(state->requestHandlesMutex);
  auto it = state->requestHandles.find(downloadId);
  if (it != state->requestHandles.end() && !it->second.empty()) {
    auto &handles = it->second;
    auto handleIt = std::find(handles.begin(), handles.end(), requestHandle);
    if (handleIt != handles.end()) {
      handles.erase(handleIt);
    }
    if (handles.empty()) {
      state->requestHandles.erase(it);
    }
  }
}

HINTERNET DownloadEngine::GetTrackedHandle(
    const std::shared_ptr<EngineState> &state, int downloadId) {
  // Deprecated - use CloseTrackedHandles instead
  return nullptr;
}

bool DownloadEngine::MergeChunkFiles(
    const std::vector<std::string> &partPaths,
    const std::string &outputPath) {
  std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    return false;
  }

  char buffer[65536];
  for (const auto &partPath : partPaths) {
    std::ifstream input(partPath, std::ios::binary);
    if (!input.is_open()) {
      output.close();
      DeleteFileA(outputPath.c_str());  // Clean up partial output
      return false;
    }

    while (input) {
      input.read(buffer, sizeof(buffer));
      std::streamsize readCount = input.gcount();
      if (readCount > 0) {
        output.write(buffer, readCount);
        if (output.fail()) {
          output.close();
          DeleteFileA(outputPath.c_str());  // Clean up partial output
          return false;
        }
      }
      if (input.eof()) break;
      if (input.bad()) {
        output.close();
        DeleteFileA(outputPath.c_str());  // Clean up partial output
        return false;
      }
    }
  }

  return true;
}

bool DownloadEngine::PerformMultiSegmentDownload(
    std::shared_ptr<EngineState> state, std::shared_ptr<Download> download,
    int connections) {
  if (!state || !download || !state->running.load())
    return false;

  std::string savePath = download->GetSavePath();
  CreateDirectoryA(savePath.c_str(), NULL);
  std::string filePath = savePath + "\\" + download->GetFilename();

  auto chunks = download->GetChunksCopy();
  if (chunks.empty()) {
    download->SetStatus(DownloadStatus::Error);
    download->SetErrorMessage("Invalid chunk configuration");
    return false;
  }

  std::vector<std::string> partPaths;
  partPaths.reserve(chunks.size());

  int64_t fileSize = download->GetTotalSize();
  if (fileSize <= 0) {
    download->SetStatus(DownloadStatus::Error);
    download->SetErrorMessage("Unknown file size for multi-segment download");
    return false;
  }

  // Resume existing parts if present
  for (size_t i = 0; i < chunks.size(); ++i) {
    std::string partPath = filePath + ".part" + std::to_string(i);
    partPaths.push_back(partPath);

    int64_t partSize = GetExistingFileSize(partPath);
    int64_t chunkLength = (chunks[i].endByte - chunks[i].startByte) + 1;
    if (partSize > 0) {
      if (partSize > chunkLength) {
        // Part file is corrupted (larger than expected), delete and restart
        DeleteFileA(partPath.c_str());
        partSize = 0;
      }
      chunks[i].currentByte = chunks[i].startByte + partSize;
      chunks[i].completed = (partSize == chunkLength);
    } else {
      chunks[i].currentByte = chunks[i].startByte;
      chunks[i].completed = false;
    }
  }
  download->SetChunks(chunks);

  std::vector<std::future<ChunkResult>> futures;
  futures.reserve(chunks.size());
  std::vector<bool> futureReady(chunks.size(), false);

  int64_t totalSpeedLimit = state->speedLimitBytes.load();
  int64_t perConnectionLimit = 0;
  if (totalSpeedLimit > 0 && connections > 0) {
    perConnectionLimit = totalSpeedLimit / connections;
    if (perConnectionLimit < 1024) {
      perConnectionLimit = 1024;
    }
  }

  // Track start time and initial progress for speed calculation
  auto downloadStartTime = std::chrono::steady_clock::now();
  int64_t initialDownloaded = download->GetDownloadedSize();

  for (size_t i = 0; i < chunks.size(); ++i) {
    futures.push_back(std::async(std::launch::async,
                                 [state, download, i, partPaths,
                                  perConnectionLimit]() {
                                   // Get fresh chunk state from download
                                   auto freshChunks = download->GetChunksCopy();
                                   if (i >= freshChunks.size() ||
                                       freshChunks[i].completed) {
                                     return ChunkResult::Success;
                                   }
                                   int attempt = 0;
                                   while (attempt <= Config::MAX_CHUNK_RETRIES) {
                                      // Refresh chunk state on each retry attempt
                                      freshChunks = download->GetChunksCopy();
                                      if (i >= freshChunks.size()) {
                                        return ChunkResult::Failed;
                                      }
                                      int64_t fileOffset = 0;
                                      if (freshChunks[i].currentByte > freshChunks[i].startByte) {
                                        fileOffset = freshChunks[i].currentByte - freshChunks[i].startByte;
                                      }
                                      int64_t start =
                                          freshChunks[i].currentByte > freshChunks[i].startByte
                                              ? freshChunks[i].currentByte
                                              : freshChunks[i].startByte;
                                      ChunkResult result = PerformChunkDownload(
                                          state, download, static_cast<int>(i),
                                          start, freshChunks[i].endByte,
                                          partPaths[i], perConnectionLimit,
                                          fileOffset);
                                     if (result == ChunkResult::Success ||
                                         result == ChunkResult::RangeUnsupported ||
                                         result == ChunkResult::Aborted) {
                                       return result;
                                     }
                                     if (result == ChunkResult::Throttled) {
                                       std::this_thread::sleep_for(
                                           std::chrono::milliseconds(
                                               Config::BASE_CHUNK_RETRY_MS *
                                               (attempt + 1)));
                                     } else {
                                       std::this_thread::sleep_for(
                                           std::chrono::milliseconds(
                                               Config::BASE_CHUNK_RETRY_MS *
                                               (1 << attempt)));
                                     }
                                     attempt++;
                                   }
                                   return ChunkResult::Failed;
                                 }));
  }

  // Monitor progress and calculate aggregate speed while chunks download
  ProgressCallback progressCallback;
  {
    std::lock_guard<std::mutex> lock(state->callbackMutex);
    progressCallback = state->progressCallback;
  }

  auto lastSpeedUpdate = std::chrono::steady_clock::now();
  int64_t lastDownloaded = initialDownloaded;

  bool allComplete = false;
  while (!allComplete) {
    allComplete = true;
    for (size_t i = 0; i < futures.size(); ++i) {
      if (futureReady[i]) {
        continue;
      }
      if (futures[i].valid() &&
          futures[i].wait_for(std::chrono::milliseconds(100)) !=
              std::future_status::ready) {
        allComplete = false;
      } else {
        futureReady[i] = true;
      }
    }

    // Calculate and update aggregate speed
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now - lastSpeedUpdate)
                       .count();
    if (elapsed >= Config::SPEED_UPDATE_INTERVAL_MS) {
      int64_t currentDownloaded = download->GetDownloadedSize();
      double speed = 0.0;
      if (elapsed > 0) {
        speed = static_cast<double>(currentDownloaded - lastDownloaded) /
                (elapsed / 1000.0);
      }
      if (speed >= 0.0) {
        download->SetSpeed(speed);
      }
      lastSpeedUpdate = now;
      lastDownloaded = currentDownloaded;

      if (progressCallback) {
        progressCallback(download->GetId(), currentDownloaded,
                         download->GetTotalSize(), speed);
      }
    }

    // Check for cancellation
    if (download->GetStatus() == DownloadStatus::Cancelled ||
        download->GetStatus() == DownloadStatus::Paused ||
        !state->running.load()) {
      break;
    }
  }

  bool allOk = true;
  bool rangeUnsupported = false;
  bool throttled = false;
  bool networkError = false;
  for (size_t i = 0; i < futures.size(); ++i) {
    if (futures[i].valid()) {
      ChunkResult result = futures[i].get();
      if (result != ChunkResult::Success) {
        allOk = false;
        if (result == ChunkResult::RangeUnsupported) {
          rangeUnsupported = true;
        }
        if (result == ChunkResult::Throttled) {
          throttled = true;
        }
        if (result == ChunkResult::NetworkError || result == ChunkResult::Failed) {
          networkError = true;
        }
      }
    } else {
      allOk = false;
    }
  }

  if (!allOk || download->GetStatus() == DownloadStatus::Cancelled ||
      download->GetStatus() == DownloadStatus::Paused) {
    if (rangeUnsupported) {
      // Range not supported - must restart from scratch
      for (const auto &partPath : partPaths) {
        DeleteFileA(partPath.c_str());
      }
      download->InitializeChunks(1);
      download->SetDownloadedSize(0);
      return PerformDownload(state, download);
    }
    if (throttled && connections > 1) {
      // Server throttling - reduce connections and restart
      for (const auto &partPath : partPaths) {
        DeleteFileA(partPath.c_str());
      }
      int reducedConnections = std::max(1, connections / 2);
      download->InitializeChunks(reducedConnections);
      download->SetDownloadedSize(0);
      return PerformMultiSegmentDownload(state, download, reducedConnections);
    }
    
    // For paused/cancelled, keep part files for resume
    if (download->GetStatus() == DownloadStatus::Paused ||
        download->GetStatus() == DownloadStatus::Cancelled) {
      // Don't delete part files - they can be resumed later
      return false;
    }
    
    // Auto-retry on network errors (with exponential backoff)
    int retryCount = download->GetRetryCount();
    if (networkError && retryCount < Config::MAX_DOWNLOAD_RETRIES) {
      std::cerr << "[Download] Auto-retry " << (retryCount + 1) 
                << "/" << Config::MAX_DOWNLOAD_RETRIES << " after network error" << std::endl;
      download->IncrementRetry();
      
      // Exponential backoff
      int delayMs = Config::BASE_DOWNLOAD_RETRY_MS * (1 << std::min(retryCount, 4));
      std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
      
      // Check if cancelled during wait
      if (download->GetStatus() == DownloadStatus::Cancelled ||
          download->GetStatus() == DownloadStatus::Paused ||
          !state->running.load()) {
        return false;
      }
      
      download->SetStatus(DownloadStatus::Downloading);
      return PerformMultiSegmentDownload(state, download, connections);
    }
    
    // For other failures, also keep part files to allow resume
    // Only set error status, don't delete progress
    download->SetStatus(DownloadStatus::Error);
    std::string errorMsg = "Download failed after " + 
                           std::to_string(download->GetRetryCount()) + " retries";
    download->SetErrorMessage(errorMsg);
    std::cerr << "[Download] " << errorMsg << std::endl;
    return false;
  }

  if (!MergeChunkFiles(partPaths, filePath)) {
    for (const auto &partPath : partPaths) {
      DeleteFileA(partPath.c_str());
    }
    download->SetStatus(DownloadStatus::Error);
    download->SetErrorMessage("Failed to merge download parts");
    return false;
  }

  int64_t mergedSize = GetExistingFileSize(filePath);
  if (download->GetTotalSize() > 0 &&
      mergedSize != download->GetTotalSize()) {
    for (const auto &partPath : partPaths) {
      DeleteFileA(partPath.c_str());
    }
    download->SetStatus(DownloadStatus::Error);
    download->SetErrorMessage("Merged file size mismatch");
    return false;
  }

  for (const auto &partPath : partPaths) {
    DeleteFileA(partPath.c_str());
  }

  download->SetStatus(DownloadStatus::Completed);
  download->ResetRetry();

  CompletionCallback completionCallback;
  {
    std::lock_guard<std::mutex> lock(state->callbackMutex);
    completionCallback = state->completionCallback;
  }
  if (completionCallback)
    completionCallback(download->GetId(), true, "");

  return true;
}

void DownloadEngine::CleanupCompletedDownloads() {
  std::lock_guard<std::mutex> lock(m_activeDownloadsMutex);
  m_activeDownloads.erase(
      std::remove_if(m_activeDownloads.begin(), m_activeDownloads.end(),
                     [](std::future<bool> &f) {
                       return f.valid() &&
                              f.wait_for(std::chrono::seconds(0)) ==
                                  std::future_status::ready;
                     }),
      m_activeDownloads.end());
}

