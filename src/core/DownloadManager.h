#pragma once

#include "Download.h"
#include "DownloadEngine.h"
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <wx/datetime.h>
#include <wx/timer.h>


class DownloadManager : public wxEvtHandler {
public:
  static DownloadManager &GetInstance();

  // Disable copy
  DownloadManager(const DownloadManager &) = delete;
  DownloadManager &operator=(const DownloadManager &) = delete;

  // Download management
  int AddDownload(const std::string &url, const std::string &savePath = "");
  void RemoveDownload(int downloadId, bool deleteFile = false);
  void StartDownload(int downloadId);
  void PauseDownload(int downloadId);
  void ResumeDownload(int downloadId);
  void CancelDownload(int downloadId);

  // Batch operations
  void StartAllDownloads();
  void PauseAllDownloads();
  void CancelAllDownloads();

  // Queue management
  void StartQueue();
  void StopQueue();
  bool IsQueueRunning() const { return m_isQueueRunning; }
  void ProcessQueue();

  // Scheduling
  void SetSchedule(bool enableStart, const wxDateTime &startTime,
                   bool enableStop, const wxDateTime &stopTime,
                   int maxConcurrent, bool hangUp, bool exitApp, bool shutdown);
  void CheckSchedule();

  // Query downloads
  std::shared_ptr<Download> GetDownload(int downloadId) const;
  std::vector<std::shared_ptr<Download>> GetAllDownloads() const;
  std::vector<std::shared_ptr<Download>>
  GetDownloadsByCategory(const std::string &category) const;
  std::vector<std::shared_ptr<Download>>
  GetDownloadsByStatus(DownloadStatus status) const;

  // Statistics
  int GetTotalDownloads() const;
  int GetActiveDownloads() const;
  double GetTotalSpeed() const;

  // Settings
  void SetMaxSimultaneousDownloads(int max) {
    m_maxSimultaneousDownloads = max;
  }
  void SetDefaultSavePath(const std::string &path) { m_defaultSavePath = path; }

  // Callbacks for UI updates
  using DownloadUpdateCallback = std::function<void(int downloadId)>;
  void SetUpdateCallback(DownloadUpdateCallback callback) {
    m_updateCallback = callback;
  }

private:
  DownloadManager();
  ~DownloadManager();

  std::vector<std::shared_ptr<Download>> m_downloads;
  mutable std::mutex m_downloadsMutex;

  // Queue & Schedule state
  bool m_isQueueRunning;
  wxTimer *m_schedulerTimer;

  // Schedule settings
  bool m_schedStartEnabled;
  wxDateTime m_schedStartTime;
  bool m_schedStopEnabled;
  wxDateTime m_schedStopTime;
  bool m_schedHangUp;
  bool m_schedExit;
  bool m_schedShutdown;

  // Internal event handler for timer
  void OnSchedulerTimer(wxTimerEvent &event);

  std::unique_ptr<DownloadEngine> m_engine;

  int m_nextId;
  int m_maxSimultaneousDownloads;
  std::string m_defaultSavePath;

  DownloadUpdateCallback m_updateCallback;

  // Database persistence helpers
  void LoadDownloadsFromDatabase();
  void SaveAllDownloadsToDatabase();
  void SaveDownloadToDatabase(int downloadId);

  // Folder management
  void EnsureCategoryFoldersExist();

  void OnDownloadProgress(int downloadId, int64_t downloaded, int64_t total,
                          double speed);
  void OnDownloadComplete(int downloadId, bool success,
                          const std::string &error);
};
