#pragma once

#include "Download.h"
#include <atomic>
#include <curl/curl.h>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class DownloadEngine {
public:
  DownloadEngine();
  ~DownloadEngine();

  // Disable copy
  DownloadEngine(const DownloadEngine &) = delete;
  DownloadEngine &operator=(const DownloadEngine &) = delete;

  // Start downloading a file
  bool StartDownload(std::shared_ptr<Download> download);

  // Pause/resume/cancel
  void PauseDownload(std::shared_ptr<Download> download);
  void ResumeDownload(std::shared_ptr<Download> download);
  void CancelDownload(std::shared_ptr<Download> download);

  // Get file info (size, resumable) without downloading
  bool GetFileInfo(const std::string &url, int64_t &fileSize, bool &resumable);

  // Callbacks for progress updates
  using ProgressCallback = std::function<void(
      int downloadId, int64_t downloaded, int64_t total, double speed)>;
  using CompletionCallback = std::function<void(int downloadId, bool success,
                                                const std::string &error)>;

  void SetProgressCallback(ProgressCallback callback) {
    m_progressCallback = callback;
  }
  void SetCompletionCallback(CompletionCallback callback) {
    m_completionCallback = callback;
  }

  // Settings
  void SetMaxConnections(int connections) { m_maxConnections = connections; }
  void SetSpeedLimit(int64_t bytesPerSecond) { m_speedLimit = bytesPerSecond; }
  void SetUserAgent(const std::string &userAgent) { m_userAgent = userAgent; }
  void SetProxy(const std::string &proxyHost, int proxyPort);
  void SetSSLVerification(bool verify) { m_verifySSL = verify; }
  bool GetSSLVerification() const { return m_verifySSL; }

  // CA bundle configuration for proper SSL verification
  void SetCABundlePath(const std::string &path) { m_caBundlePath = path; }
  std::string GetCABundlePath() const { return m_caBundlePath; }
  void SetUseNativeCAStore(bool use) { m_useNativeCAStore = use; }
  bool GetUseNativeCAStore() const { return m_useNativeCAStore; }

private:
  // CURL handles
  CURLM *m_multiHandle;

  // Settings
  int m_maxConnections;
  int64_t m_speedLimit;
  std::string m_userAgent;
  std::string m_proxyUrl;
  bool m_verifySSL;           // SSL peer verification setting
  std::string m_caBundlePath; // Path to CA certificate bundle (cacert.pem)
  bool m_useNativeCAStore; // Use Windows native CA store (CURLSSLOPT_NATIVE_CA)

  // Callbacks
  ProgressCallback m_progressCallback;
  CompletionCallback m_completionCallback;

  // Active download tracking for safe thread management
  std::vector<std::future<bool>> m_activeDownloads;
  std::mutex m_activeDownloadsMutex;

  // Worker thread
  std::thread m_workerThread;
  std::atomic<bool> m_running;

  // Cleanup completed futures
  void CleanupCompletedDownloads();

  // Helper methods
  bool PerformDownload(std::shared_ptr<Download> download);
  bool DownloadChunk(std::shared_ptr<Download> download, int chunkIndex,
                     CURL *easyHandle);

  // CURL callbacks
  static size_t WriteCallback(void *ptr, size_t size, size_t nmemb,
                              void *userdata);
  static int ProgressCallback_CURL(void *clientp, curl_off_t dltotal,
                                   curl_off_t dlnow, curl_off_t ultotal,
                                   curl_off_t ulnow);

  // Initialize CURL with common options
  CURL *CreateEasyHandle(const std::string &url);
};
