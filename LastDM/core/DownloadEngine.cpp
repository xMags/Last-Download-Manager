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
} // namespace Config

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
  return m_entry ? m_entry->handle : nullptr;
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

bool DownloadEngine::StartDownload(std::shared_ptr<Download> download) {
  if (!download)
    return false;

  auto state = m_state;
  if (!state || !state->running.load())
    return false;

  int64_t fileSize;
  bool resumable;

  if (GetFileInfo(download->GetUrl(), fileSize, resumable)) {
    download->SetTotalSize(fileSize);
  }

  // For this simple WinINet implementation, we stick to 1 connection per
  // download for simplicity unless we implement complex range merging which is
  // complex with raw WinINet API. The original code had multi-chunk logic but
  // was complex. We will preserve the single-stream logic for stability in this
  // migration.

  download->InitializeChunks(1);
  download->SetStatus(DownloadStatus::Downloading);
  download->UpdateLastTryTime();

  CleanupCompletedDownloads();

  {
    std::lock_guard<std::mutex> lock(m_activeDownloadsMutex);
    m_activeDownloads.push_back(std::async(std::launch::async,
                                           [state, download]() {
                                             return PerformDownload(state,
                                                                    download);
                                           }));
  }

  return true;
}

void DownloadEngine::PauseDownload(std::shared_ptr<Download> download) {
  if (download) {
    download->SetStatus(DownloadStatus::Paused);
  }
}

void DownloadEngine::ResumeDownload(std::shared_ptr<Download> download) {
  if (!download)
    return;
  if (download->GetStatus() == DownloadStatus::Completed)
    return;
  if (download->GetStatus() == DownloadStatus::Paused) {
    StartDownload(download);
  }
}

void DownloadEngine::CancelDownload(std::shared_ptr<Download> download) {
  if (download) {
    download->SetStatus(DownloadStatus::Cancelled);
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
    } else {
      bool validHost = true;
      for (char c : proxyHost) {
        if (std::isspace(static_cast<unsigned char>(c))) {
          validHost = false;
          break;
        }
      }

      if (!validHost) {
        std::cerr << "Invalid proxy host format: " << proxyHost << std::endl;
      } else {
        newProxyUrl = proxyHost + ":" + std::to_string(proxyPort);
      }
    }
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
  int64_t existingSize = 0;
  std::ifstream checkFile(filePath, std::ios::binary | std::ios::ate);
  if (checkFile.is_open()) {
    existingSize = checkFile.tellg();
    checkFile.close();
  }

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
      headers.empty() ? -1 : headers.length(), flags, 0);

  if (!hUrl) {
    download->SetStatus(DownloadStatus::Error);
    download->SetErrorMessage("Failed to open URL. Error: " +
                              std::to_string(GetLastError()));

    // Retry Logic
    if (download->ShouldRetry()) {
      download->IncrementRetry();
      std::this_thread::sleep_for(
          std::chrono::milliseconds(download->GetRetryDelayMs()));
      if (download->GetStatus() == DownloadStatus::Cancelled)
        return false;
      download->SetStatus(DownloadStatus::Queued);
      return PerformDownload(state, download);
    }

    if (completionCallback)
      completionCallback(download->GetId(), false, "Connection failed");
    return false;
  }

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
      InternetCloseHandle(hUrl);
      shouldResume = false;
      existingSize = 0;
      download->SetDownloadedSize(0);
      headers.clear();

      hUrl = InternetOpenUrlA(hSession, url.c_str(), NULL, -1, flags, 0);
      if (!hUrl) {
        download->SetStatus(DownloadStatus::Error);
        download->SetErrorMessage("Failed to restart download. Error: " +
                                  std::to_string(GetLastError()));
        if (completionCallback)
          completionCallback(download->GetId(), false, "Connection failed");
        return false;
      }
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
    InternetCloseHandle(hUrl);
    download->SetStatus(DownloadStatus::Error);
    download->SetErrorMessage("File I/O Error");
    return false;
  }

  // Read Loop
  DWORD bytesRead = 0;
  char buffer[8192]; // 8KB buffer
  auto lastSpeedUpdate = std::chrono::steady_clock::now();
  auto lastThrottleUpdate = lastSpeedUpdate;
  int64_t lastBytes = shouldResume ? existingSize : 0;

  do {
    // Check Status
    if (!state->running.load() ||
        download->GetStatus() == DownloadStatus::Cancelled ||
        download->GetStatus() == DownloadStatus::Paused) {
      file.close();
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
        if (elapsed >= 500) {
          double speed =
              static_cast<double>(currentSize - lastBytes) / (elapsed / 1000.0);
          download->SetSpeed(speed);
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
      // Read Error
      file.close();
      InternetCloseHandle(hUrl);
      download->SetStatus(DownloadStatus::Error);
      download->SetErrorMessage("Read Error: " +
                                std::to_string(GetLastError()));
      if (completionCallback)
        completionCallback(download->GetId(), false, "Read Error");
      return false;
    }

  } while (bytesRead > 0);

  file.close();
  InternetCloseHandle(hUrl);

  download->SetStatus(DownloadStatus::Completed);
  download->ResetRetry();
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

