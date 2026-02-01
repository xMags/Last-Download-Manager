#pragma once

#include "Download.h"
#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <wininet.h>

#pragma comment(lib, "wininet.lib")

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

  void SetProgressCallback(ProgressCallback callback);
  void SetCompletionCallback(CompletionCallback callback);

  // Settings
  void SetMaxConnections(int connections) { m_maxConnections = connections; }
  void SetSpeedLimit(int64_t bytesPerSecond);
  void SetUserAgent(const std::string &userAgent);
  void SetProxy(const std::string &proxyHost, int proxyPort);
  void SetSSLVerification(bool verify);
  bool GetSSLVerification() const;

  // CA bundle configuration (No longer needed for WinINet, kept for API compatibility if needed, but ignored)
  void SetCABundlePath(const std::string &path) { m_caBundlePath = path; }
  std::string GetCABundlePath() const { return m_caBundlePath; }
  void SetUseNativeCAStore(bool use) { m_useNativeCAStore = use; }
  bool GetUseNativeCAStore() const { return m_useNativeCAStore; }

private:
  struct SessionEntry {
    HINTERNET handle = nullptr;
    std::atomic<int> activeCount{0};
    std::atomic<bool> closing{false};
    std::mutex handleMutex;
  };

  struct EngineState {
    std::mutex sessionMutex;
    std::shared_ptr<SessionEntry> session;
    std::vector<std::shared_ptr<SessionEntry>> retiredSessions;

    std::atomic<bool> running{false};
    std::atomic<int64_t> speedLimitBytes{0};
    std::string userAgent;
    std::string proxyUrl;
    std::atomic<bool> verifySSL{true};

    std::mutex callbackMutex;
    ProgressCallback progressCallback;
    CompletionCallback completionCallback;
  };

  struct SessionUsage {
    explicit SessionUsage(std::shared_ptr<SessionEntry> entry);
    ~SessionUsage();
    HINTERNET handle() const;

  private:
    std::shared_ptr<SessionEntry> m_entry;
  };

  // Settings
  int m_maxConnections;
  std::string m_caBundlePath;
  bool m_useNativeCAStore;

  std::shared_ptr<EngineState> m_state;

  // Active download tracking for safe thread management
  std::vector<std::future<bool>> m_activeDownloads;
  std::mutex m_activeDownloadsMutex;

  // Worker thread
  std::thread m_workerThread;
  // Cleanup completed futures
  void CleanupCompletedDownloads();

  static void ConfigureSessionTimeouts(HINTERNET session);
  static HINTERNET OpenSession(const std::string &userAgent,
                               const std::string &proxyUrl);
  static void CloseSessionHandle(const std::shared_ptr<SessionEntry> &entry);
  static void CloseSessionIfIdle(const std::shared_ptr<SessionEntry> &entry);
  static bool ParseContentRangeStart(const std::string &value,
                                     int64_t &startOut);

  static void CleanupRetiredSessions(
      const std::shared_ptr<EngineState> &state);

  bool ReinitializeSession(const std::string &proxyUrl);

  // Helper methods
  static bool PerformDownload(std::shared_ptr<EngineState> state,
                              std::shared_ptr<Download> download);
  
};
