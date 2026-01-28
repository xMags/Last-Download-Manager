#include "DownloadEngine.h"
#include <Windows.h>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>

// Named constants for CURL configuration (previously magic numbers)
namespace CurlConfig {
constexpr long CONNECT_TIMEOUT_SECONDS = 30; // Connection timeout
constexpr long REQUEST_TIMEOUT_SECONDS = 0;  // 0 = no timeout for full request
constexpr long HEAD_REQUEST_TIMEOUT_SECONDS = 30; // Timeout for HEAD requests
constexpr long LOW_SPEED_LIMIT_BYTES = 1;   // Minimum bytes/sec before timeout
constexpr long LOW_SPEED_TIME_SECONDS = 60; // Seconds to wait at low speed
constexpr long SPEED_UPDATE_INTERVAL_MS = 500; // Speed calculation interval
} // namespace CurlConfig

// Structure to pass data to CURL callbacks (per-download to avoid race
// conditions)
struct WriteData {
  std::shared_ptr<Download> download;
  int chunkIndex;
  std::ofstream *file;
  DownloadEngine *engine;
  std::chrono::steady_clock::time_point lastSpeedUpdate; // Per-download timing
  int64_t lastBytes; // Per-download byte counter
  bool writeError;   // Track write failures
};

DownloadEngine::DownloadEngine()
    : m_multiHandle(nullptr), m_maxConnections(8), m_speedLimit(0),
      m_userAgent("LastDownloadManager/1.0"), m_running(false),
      m_verifySSL(true),         // SSL verification enabled by default (secure)
      m_useNativeCAStore(true) { // Use Windows native CA store by default
  // Initialize CURL globally (should be called once)
  curl_global_init(CURL_GLOBAL_ALL);
  m_multiHandle = curl_multi_init();

  // Try to find a bundled CA certificate file as fallback
  // Look in app directory first, then current working directory
  const char *possiblePaths[] = {"resources/cacert.pem", "cacert.pem",
                                 "../resources/cacert.pem"};

  for (const char *path : possiblePaths) {
    std::ifstream file(path);
    if (file.good()) {
      m_caBundlePath = path;
      break;
    }
  }
}

DownloadEngine::~DownloadEngine() {
  m_running = false;

  // Wait for all active downloads to complete (with timeout)
  {
    std::lock_guard<std::mutex> lock(m_activeDownloadsMutex);
    for (auto &future : m_activeDownloads) {
      if (future.valid()) {
        // Wait up to 2 seconds for each download to stop gracefully
        auto status = future.wait_for(std::chrono::seconds(2));
        // If still running after timeout, it will be abandoned (curl will
        // abort)
      }
    }
    m_activeDownloads.clear();
  }

  if (m_workerThread.joinable()) {
    m_workerThread.join();
  }

  if (m_multiHandle) {
    curl_multi_cleanup(m_multiHandle);
  }

  curl_global_cleanup();
}

bool DownloadEngine::GetFileInfo(const std::string &url, int64_t &fileSize,
                                 bool &resumable) {
  CURL *curl = curl_easy_init();
  if (!curl)
    return false;

  // Buffer to store Accept-Ranges header
  std::string acceptRangesValue;

  // Header callback to capture Accept-Ranges
  auto headerCallback = [](char *buffer, size_t size, size_t nitems,
                           void *userdata) -> size_t {
    std::string *acceptRanges = static_cast<std::string *>(userdata);
    size_t totalSize = size * nitems;
    std::string header(buffer, totalSize);

    // Check for Accept-Ranges header (case-insensitive)
    if (header.length() > 14) {
      std::string headerLower = header.substr(0, 14);
      for (auto &c : headerLower)
        c = std::tolower(c);
      if (headerLower == "accept-ranges:") {
        *acceptRanges = header.substr(14);
        // Trim whitespace
        size_t start = acceptRanges->find_first_not_of(" \t\r\n");
        size_t end = acceptRanges->find_last_not_of(" \t\r\n");
        if (start != std::string::npos && end != std::string::npos) {
          *acceptRanges = acceptRanges->substr(start, end - start + 1);
        }
      }
    }
    return totalSize;
  };

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_NOBODY, 1L); // HEAD request
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, m_userAgent.c_str());
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, m_verifySSL ? 1L : 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, m_verifySSL ? 2L : 0L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(
      curl, CURLOPT_HEADERFUNCTION,
      static_cast<size_t (*)(char *, size_t, size_t, void *)>(
          +[](char *buffer, size_t size, size_t nitems,
              void *userdata) -> size_t {
            std::string *acceptRanges = static_cast<std::string *>(userdata);
            size_t totalSize = size * nitems;
            std::string header(buffer, totalSize);

            if (header.length() > 14) {
              std::string headerLower = header.substr(0, 14);
              for (auto &c : headerLower)
                c = static_cast<char>(std::tolower(c));
              if (headerLower == "accept-ranges:") {
                *acceptRanges = header.substr(14);
                size_t start = acceptRanges->find_first_not_of(" \t\r\n");
                size_t end = acceptRanges->find_last_not_of(" \t\r\n");
                if (start != std::string::npos && end != std::string::npos) {
                  *acceptRanges = acceptRanges->substr(start, end - start + 1);
                }
              }
            }
            return totalSize;
          }));
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &acceptRangesValue);

  if (!m_proxyUrl.empty()) {
    curl_easy_setopt(curl, CURLOPT_PROXY, m_proxyUrl.c_str());
  }

  CURLcode res = curl_easy_perform(curl);

  if (res == CURLE_OK) {
    curl_off_t cl;
    res = curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
    fileSize = (res == CURLE_OK) ? cl : -1;

    // Check Accept-Ranges header - server explicitly supports byte ranges
    // Convert to lowercase for comparison
    std::string rangesLower = acceptRangesValue;
    for (auto &c : rangesLower)
      c = static_cast<char>(std::tolower(c));

    if (rangesLower.find("bytes") != std::string::npos) {
      resumable = true; // Server explicitly supports range requests
    } else if (rangesLower.find("none") != std::string::npos) {
      resumable = false; // Server explicitly doesn't support ranges
    } else {
      // No Accept-Ranges header - assume resumable if we know the size
      resumable = (fileSize > 0);
    }
  } else {
    fileSize = -1;
    resumable = false;
  }

  curl_easy_cleanup(curl);
  return (res == CURLE_OK);
}

bool DownloadEngine::StartDownload(std::shared_ptr<Download> download) {
  if (!download)
    return false;

  // Get file info first
  int64_t fileSize;
  bool resumable;

  if (GetFileInfo(download->GetUrl(), fileSize, resumable)) {
    download->SetTotalSize(fileSize);
  }

  // Initialize chunks for multi-threaded download
  int numConnections = resumable ? m_maxConnections : 1;
  download->InitializeChunks(numConnections);

  // Start download
  download->SetStatus(DownloadStatus::Downloading);
  download->UpdateLastTryTime();

  // Cleanup any completed futures first
  CleanupCompletedDownloads();

  // Start download with tracked future (not detached!)
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

  // Don't resume if download is completed
  if (download->GetStatus() == DownloadStatus::Completed)
    return;

  // Only resume if paused
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
  // Validate proxy settings (#12 - input validation)
  if (proxyHost.empty()) {
    m_proxyUrl.clear();
    return;
  }

  // Validate port range (1-65535)
  if (proxyPort <= 0 || proxyPort > 65535) {
    std::cerr << "Invalid proxy port: " << proxyPort << " (must be 1-65535)"
              << std::endl;
    m_proxyUrl.clear();
    return;
  }

  // Validate host format (basic check - no spaces, has at least one character)
  bool validHost = !proxyHost.empty();
  for (char c : proxyHost) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      validHost = false;
      break;
    }
  }

  if (!validHost) {
    std::cerr << "Invalid proxy host format: " << proxyHost << std::endl;
    m_proxyUrl.clear();
    return;
  }

  m_proxyUrl = proxyHost + ":" + std::to_string(proxyPort);
}

bool DownloadEngine::PerformDownload(std::shared_ptr<Download> download) {
  if (!download)
    return false;

  CURL *curl = CreateEasyHandle(download->GetUrl());
  if (!curl) {
    download->SetStatus(DownloadStatus::Error);
    download->SetErrorMessage("Failed to initialize CURL");
    return false;
  }

  // Ensure save directory exists
  std::string savePath = download->GetSavePath();
  CreateDirectoryA(savePath.c_str(), NULL);

  // Create output file path
  std::string filePath = savePath + "\\" + download->GetFilename();

  // Check if partial file exists for resume
  int64_t existingSize = 0;
  std::ifstream checkFile(filePath, std::ios::binary | std::ios::ate);
  if (checkFile.is_open()) {
    existingSize = checkFile.tellg();
    checkFile.close();
  }

  // Determine if we should resume
  bool shouldResume = (existingSize > 0 && download->GetDownloadedSize() > 0 &&
                       download->GetStatus() == DownloadStatus::Downloading);

  // Open file in appropriate mode
  std::ofstream file;
  if (shouldResume) {
    // Append mode for resume
    file.open(filePath, std::ios::binary | std::ios::app);
    // Set resume position in CURL
    curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE,
                     static_cast<curl_off_t>(existingSize));
  } else {
    // Truncate mode for new download
    file.open(filePath, std::ios::binary | std::ios::trunc);
    download->SetDownloadedSize(0);
  }

  if (!file.is_open()) {
    download->SetStatus(DownloadStatus::Error);
    download->SetErrorMessage("Failed to create output file: " + filePath);
    curl_easy_cleanup(curl);
    return false;
  }

  // Setup write data with per-download timing
  WriteData writeData;
  writeData.download = download;
  writeData.chunkIndex = 0;
  writeData.file = &file;
  writeData.engine = this;
  writeData.lastSpeedUpdate = std::chrono::steady_clock::now();
  writeData.lastBytes = shouldResume ? existingSize : 0;
  writeData.writeError = false;

  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &writeData);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback_CURL);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &writeData);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

  // Speed limit
  if (m_speedLimit > 0) {
    curl_easy_setopt(curl, CURLOPT_MAX_RECV_SPEED_LARGE,
                     static_cast<curl_off_t>(m_speedLimit));
  }

  // Perform download
  CURLcode res = curl_easy_perform(curl);

  file.close();
  curl_easy_cleanup(curl);

  if (res == CURLE_OK) {
    download->SetStatus(DownloadStatus::Completed);
    download->ResetRetry(); // Reset retry count on success
    if (m_completionCallback) {
      m_completionCallback(download->GetId(), true, "");
    }
    return true;
  } else {
    // Check if it was cancelled or paused by user
    if (download->GetStatus() == DownloadStatus::Cancelled ||
        download->GetStatus() == DownloadStatus::Paused) {
      if (m_completionCallback) {
        m_completionCallback(download->GetId(), false, "User cancelled/paused");
      }
      return false;
    }

    // Set error status
    download->SetStatus(DownloadStatus::Error);
    download->SetErrorMessage(curl_easy_strerror(res));

    // Check if we should auto-retry
    if (download->ShouldRetry()) {
      download->IncrementRetry();
      int delayMs = download->GetRetryDelayMs();

      std::cerr << "Download " << download->GetId() << " failed, retrying in "
                << delayMs << "ms (attempt " << download->GetRetryCount() << "/"
                << download->GetMaxRetries() << ")" << std::endl;

      // Wait for the backoff delay
      std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));

      // Check if user cancelled during wait
      if (download->GetStatus() == DownloadStatus::Cancelled) {
        if (m_completionCallback) {
          m_completionCallback(download->GetId(), false,
                               "Cancelled during retry wait");
        }
        return false;
      }

      // Reset status and retry
      download->SetStatus(DownloadStatus::Queued);
      download->UpdateLastTryTime();
      return PerformDownload(download); // Recursive retry
    }

    // No more retries - report final failure
    if (m_completionCallback) {
      std::string errorMsg = curl_easy_strerror(res);
      if (download->GetRetryCount() > 0) {
        errorMsg += " (after " + std::to_string(download->GetRetryCount()) +
                    " retries)";
      }
      m_completionCallback(download->GetId(), false, errorMsg);
    }
    return false;
  }
}

CURL *DownloadEngine::CreateEasyHandle(const std::string &url) {
  CURL *curl = curl_easy_init();
  if (!curl)
    return nullptr;

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, m_userAgent.c_str());
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, m_verifySSL ? 1L : 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, m_verifySSL ? 2L : 0L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,
                   CurlConfig::CONNECT_TIMEOUT_SECONDS);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT,
                   CurlConfig::LOW_SPEED_LIMIT_BYTES);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,
                   CurlConfig::LOW_SPEED_TIME_SECONDS);

  // Configure CA certificates for SSL verification
  if (m_verifySSL) {
    // Try to use Windows native CA store (curl 7.71.0+)
    if (m_useNativeCAStore) {
#ifdef CURLSSLOPT_NATIVE_CA
      curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
#endif
    }

    // Also set CA bundle path if available (as primary or fallback)
    if (!m_caBundlePath.empty()) {
      curl_easy_setopt(curl, CURLOPT_CAINFO, m_caBundlePath.c_str());
    }
  }

  if (!m_proxyUrl.empty()) {
    curl_easy_setopt(curl, CURLOPT_PROXY, m_proxyUrl.c_str());
  }

  return curl;
}

size_t DownloadEngine::WriteCallback(void *ptr, size_t size, size_t nmemb,
                                     void *userdata) {
  WriteData *data = static_cast<WriteData *>(userdata);
  size_t totalSize = size * nmemb;

  // Check if download was cancelled or paused
  if (data->download->GetStatus() == DownloadStatus::Cancelled ||
      data->download->GetStatus() == DownloadStatus::Paused) {
    return 0; // Return 0 to abort transfer
  }

  // Check for previous write error
  if (data->writeError) {
    return 0; // Abort if we already had an error
  }

  // Write to file and check for errors
  data->file->write(static_cast<char *>(ptr), totalSize);
  if (data->file->fail()) {
    data->writeError = true;
    data->download->SetStatus(DownloadStatus::Error);
    data->download->SetErrorMessage(
        "Disk write failed - check available disk space");
    return 0; // Return 0 to abort transfer
  }

  // Update progress
  data->download->SetDownloadedSize(data->download->GetDownloadedSize() +
                                    static_cast<int64_t>(totalSize));

  return totalSize;
}

int DownloadEngine::ProgressCallback_CURL(void *clientp, curl_off_t dltotal,
                                          curl_off_t dlnow, curl_off_t ultotal,
                                          curl_off_t ulnow) {
  WriteData *data = static_cast<WriteData *>(clientp);

  // Check if download was cancelled or paused or had write error
  if (data->download->GetStatus() == DownloadStatus::Cancelled ||
      data->download->GetStatus() == DownloadStatus::Paused ||
      data->writeError) {
    return 1; // Return non-zero to abort transfer
  }

  // Calculate speed using per-download timing (no static variables!)
  auto now = std::chrono::steady_clock::now();
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     now - data->lastSpeedUpdate)
                     .count();

  if (elapsed >= 500) { // Update every 500ms
    double speed =
        static_cast<double>(dlnow - data->lastBytes) / (elapsed / 1000.0);
    data->download->SetSpeed(speed);
    data->lastSpeedUpdate = now;
    data->lastBytes = dlnow;

    // Call progress callback
    if (data->engine->m_progressCallback) {
      data->engine->m_progressCallback(data->download->GetId(), dlnow, dltotal,
                                       speed);
    }
  }

  return 0;
}

void DownloadEngine::CleanupCompletedDownloads() {
  std::lock_guard<std::mutex> lock(m_activeDownloadsMutex);

  // Remove completed futures from the vector
  m_activeDownloads.erase(
      std::remove_if(m_activeDownloads.begin(), m_activeDownloads.end(),
                     [](std::future<bool> &f) {
                       // Check if future is ready (completed)
                       return f.valid() &&
                              f.wait_for(std::chrono::seconds(0)) ==
                                  std::future_status::ready;
                     }),
      m_activeDownloads.end());
}
