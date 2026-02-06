#pragma once

#include "Download.h"
#include "DownloadEngine.h"
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
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
  void StartDownloadWithFormat(int downloadId, const std::string &formatId);
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
  bool IsQueueRunning() const { return m_isQueueRunning.load(); }
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
  int GetMaxSimultaneousDownloads() const { return m_maxSimultaneousDownloads; }
  void SetDefaultSavePath(const std::string &path) { m_defaultSavePath = path; }
  void ApplySettings(const class Settings &settings);

  // Schedule getters for dialog
  bool IsScheduleStartEnabled() const { return m_schedStartEnabled; }
  bool IsScheduleStopEnabled() const { return m_schedStopEnabled; }
  wxDateTime GetScheduleStartTime() const { return m_schedStartTime; }
  wxDateTime GetScheduleStopTime() const { return m_schedStopTime; }
  bool IsScheduleHangUp() const { return m_schedHangUp; }
  bool IsScheduleExit() const { return m_schedExit; }
  bool IsScheduleShutdown() const { return m_schedShutdown; }

  // Callbacks for UI updates
  using DownloadUpdateCallback = std::function<void(int downloadId)>;
  void SetUpdateCallback(DownloadUpdateCallback callback) {
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_updateCallback = callback;
  }

  // Database persistence (public for periodic saves)
  void SaveAllDownloadsToDatabase();

  // Update category for matching downloads (used by UI category rename/delete)
  void UpdateDownloadsCategory(const std::string &fromCategory,
                               const std::string &toCategory);

private:
  DownloadManager();
  ~DownloadManager();

  std::vector<std::shared_ptr<Download>> m_downloads;
  std::unordered_map<int, std::shared_ptr<Download>> m_downloadIndex;  // O(1) lookup by ID
  mutable std::mutex m_downloadsMutex;
  mutable std::mutex m_callbackMutex;

  // Queue & Schedule state
  std::atomic<bool> m_isQueueRunning;
  wxTimer *m_schedulerTimer;

  // Schedule settings
  bool m_schedStartEnabled;
  wxDateTime m_schedStartTime;
  bool m_schedStopEnabled;
  wxDateTime m_schedStopTime;
  bool m_schedHangUp;
  bool m_schedExit;
  bool m_schedShutdown;
  int m_lastSchedStartMinute = -1;  // Track last triggered minute to prevent double-trigger
  int m_lastSchedStopMinute = -1;

  // Internal event handler for timer
  void OnSchedulerTimer(wxTimerEvent &event);

  std::unique_ptr<DownloadEngine> m_engine;

  int m_nextId;
  int m_maxSimultaneousDownloads;
  std::string m_defaultSavePath;

  DownloadUpdateCallback m_updateCallback;

  // Database persistence helpers
  void LoadDownloadsFromDatabase();
  void SaveDownloadToDatabase(int downloadId);

  // Folder management
  void EnsureCategoryFoldersExist();

  void OnDownloadProgress(int downloadId, int64_t downloaded, int64_t total,
                          double speed);
  void OnDownloadComplete(int downloadId, bool success,
                          const std::string &error);
};
