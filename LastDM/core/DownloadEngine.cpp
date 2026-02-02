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

DownloadEngine::DownloadEngine()
    : m_hSession(nullptr), m_maxConnections(8), m_speedLimit(0),
      m_userAgent("LastDownloadManager/1.0"), m_running(false),
      m_verifySSL(true), m_useNativeCAStore(true) {

  // Initialize WinINet
  m_hSession = InternetOpenA(m_userAgent.c_str(), INTERNET_OPEN_TYPE_PRECONFIG,
                             NULL, NULL, 0);

  // Set timeouts
  if (m_hSession) {
    DWORD timeout = Config::CONNECT_TIMEOUT_MS;
    InternetSetOption(m_hSession, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout,
                      sizeof(DWORD));
    timeout = Config::RECEIVE_TIMEOUT_MS;
    InternetSetOption(m_hSession, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout,
                      sizeof(DWORD));
  }

  m_running = (m_hSession != nullptr);
}

DownloadEngine::~DownloadEngine() {
  m_running = false;

  HINTERNET sessionToClose = nullptr;
  {
    std::lock_guard<std::mutex> lock(m_activeDownloadsMutex);
    sessionToClose = m_hSession;
    m_hSession = nullptr;
  }

  if (sessionToClose) {
    InternetCloseHandle(sessionToClose);
  }

  std::vector<std::future<bool>> activeDownloads;
  {
    std::lock_guard<std::mutex> lock(m_activeDownloadsMutex);
    activeDownloads.swap(m_activeDownloads);
  }

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  for (auto &future : activeDownloads) {
    if (!future.valid()) {
      continue;
    }

    auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      break;
    }

    auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    future.wait_for(remaining);
  }

  if (m_workerThread.joinable()) {
    m_workerThread.join();
  }

  m_hSession = nullptr;
}

bool DownloadEngine::GetFileInfo(const std::string &url, int64_t &fileSize,
                                 bool &resumable) {
  if (!m_hSession)
    return false;

  HINTERNET hFile = InternetOpenUrlA(
      m_hSession, url.c_str(), "Head: Trigger", -1,
      INTERNET_FLAG_NO_UI | INTERNET_FLAG_RELOAD |
          (m_verifySSL ? 0
                       : INTERNET_FLAG_IGNORE_CERT_CN_INVALID |
                             INTERNET_FLAG_IGNORE_CERT_DATE_INVALID),
      0);

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
    m_activeDownloads.push_back(std::async(
        std::launch::async, &DownloadEngine::PerformDownload, this, download));
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
  if (proxyHost.empty()) {
    m_proxyUrl.clear();
    ReinitializeSession();
    return;
  }

  if (proxyPort <= 0 || proxyPort > 65535) {
    std::cerr << "Invalid proxy port: " << proxyPort
              << " (must be 1-65535)" << std::endl;
    m_proxyUrl.clear();
    ReinitializeSession();
    return;
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
    m_proxyUrl.clear();
    ReinitializeSession();
    return;
  }

  m_proxyUrl = proxyHost + ":" + std::to_string(proxyPort);
  if (!ReinitializeSession()) {
    std::cerr << "Failed to apply proxy settings" << std::endl;
  }
}

bool DownloadEngine::ReinitializeSession() {
  HINTERNET newSession = InternetOpenA(
      m_userAgent.c_str(),
      m_proxyUrl.empty() ? INTERNET_OPEN_TYPE_PRECONFIG
                         : INTERNET_OPEN_TYPE_PROXY,
      m_proxyUrl.empty() ? NULL : m_proxyUrl.c_str(), NULL, 0);

  if (!newSession) {
    m_running = false;
    return false;
  }

  DWORD timeout = Config::CONNECT_TIMEOUT_MS;
  InternetSetOption(newSession, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout,
                    sizeof(DWORD));
  timeout = Config::RECEIVE_TIMEOUT_MS;
  InternetSetOption(newSession, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout,
                    sizeof(DWORD));

  HINTERNET oldSession = nullptr;
  {
    std::lock_guard<std::mutex> lock(m_activeDownloadsMutex);
    oldSession = m_hSession;
    m_hSession = newSession;
  }

  if (oldSession) {
    InternetCloseHandle(oldSession);
  }

  m_running = true;
  return true;
}

bool DownloadEngine::PerformDownload(std::shared_ptr<Download> download) {
  if (!download || !m_hSession || !m_running)
    return false;

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
    headers = "Range: bytes=" + std::to_string(existingSize) + "-";
  } else {
    existingSize = 0;
    download->SetDownloadedSize(0);
  }

  // Open Request
  DWORD flags = INTERNET_FLAG_NO_UI | INTERNET_FLAG_RELOAD |
                INTERNET_FLAG_KEEP_CONNECTION;
  if (!m_verifySSL)
    flags |= (INTERNET_FLAG_IGNORE_CERT_CN_INVALID |
              INTERNET_FLAG_IGNORE_CERT_DATE_INVALID);

  HINTERNET hUrl = InternetOpenUrlA(
      m_hSession, url.c_str(), headers.empty() ? NULL : headers.c_str(),
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
      return PerformDownload(download);
    }

    if (m_completionCallback)
      m_completionCallback(download->GetId(), false, "Connection failed");
    return false;
  }

  if (shouldResume) {
    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (HttpQueryInfoA(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                       &statusCode, &statusSize, NULL)) {
      if (statusCode != 206) {
        InternetCloseHandle(hUrl);
        shouldResume = false;
        existingSize = 0;
        download->SetDownloadedSize(0);
        headers.clear();

        hUrl = InternetOpenUrlA(m_hSession, url.c_str(), NULL, -1, flags, 0);
        if (!hUrl) {
          download->SetStatus(DownloadStatus::Error);
          download->SetErrorMessage("Failed to restart download. Error: " +
                                    std::to_string(GetLastError()));
          if (m_completionCallback)
            m_completionCallback(download->GetId(), false,
                                 "Connection failed");
          return false;
        }
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
    if (!m_running || download->GetStatus() == DownloadStatus::Cancelled ||
        download->GetStatus() == DownloadStatus::Paused) {
      file.close();
      InternetCloseHandle(hUrl);
      if (m_running && m_completionCallback)
        m_completionCallback(download->GetId(), false, "User Aborted");
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
          if (m_completionCallback)
            m_completionCallback(download->GetId(), false, "File I/O Error");
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

          if (m_progressCallback) {
            m_progressCallback(download->GetId(), currentSize,
                               download->GetTotalSize(), speed);
          }
        }

        // Simple speed limit (blocking)
        if (m_speedLimit > 0) {
          auto now = std::chrono::steady_clock::now();
          auto elapsedMs =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  now - lastThrottleUpdate)
                  .count();
          double targetMs = (bytesRead * 1000.0) / m_speedLimit;
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
      if (m_completionCallback)
        m_completionCallback(download->GetId(), false, "Read Error");
      return false;
    }

  } while (bytesRead > 0);

  file.close();
  InternetCloseHandle(hUrl);

  download->SetStatus(DownloadStatus::Completed);
  download->ResetRetry();
  if (m_completionCallback)
    m_completionCallback(download->GetId(), true, "");

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

