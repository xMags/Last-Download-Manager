#include "DownloadManager.h"
#include "../database/DatabaseManager.h"
#include "../utils/Settings.h"
#include <Shlobj.h>
#include <algorithm>
#include <iostream>
#include <wx/app.h>
#include <wx/msgdlg.h>

// URL validation helper
static bool IsValidUrl(const std::string &url) {
  if (url.empty() || url.length() < 10)
    return false;

  // Check for valid protocol
  if (url.find("http://") != 0 && url.find("https://") != 0 &&
      url.find("ftp://") != 0) {
    return false;
  }

  return true;
}

DownloadManager &DownloadManager::GetInstance() {
  static DownloadManager instance;
  return instance;
}

DownloadManager::DownloadManager()
    : m_nextId(1), m_maxSimultaneousDownloads(3), m_isQueueRunning(false),
      m_schedulerTimer(nullptr), m_schedStartEnabled(false),
      m_schedStopEnabled(false), m_schedHangUp(false), m_schedExit(false),
      m_schedShutdown(false) {
  m_engine = std::make_unique<DownloadEngine>();

  // Set default save path to Downloads folder
  char path[MAX_PATH];
  if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, 0, path))) {
    m_defaultSavePath = std::string(path) + "\\Downloads";
  } else {
    m_defaultSavePath = "C:\\Downloads";
  }

  // Create category folders on startup
  EnsureCategoryFoldersExist();

  // Setup callbacks
  m_engine->SetProgressCallback(
      [this](int id, int64_t dl, int64_t total, double speed) {
        OnDownloadProgress(id, dl, total, speed);
      });

  m_engine->SetCompletionCallback(
      [this](int id, bool success, const std::string &error) {
        OnDownloadComplete(id, success, error);
      });

  // Load downloads from database
  LoadDownloadsFromDatabase();

  // Create scheduler timer
  m_schedulerTimer = new wxTimer(this);
  Bind(wxEVT_TIMER, &DownloadManager::OnSchedulerTimer, this,
       m_schedulerTimer->GetId());
  m_schedulerTimer->Start(1000); // Check every second
}

DownloadManager::~DownloadManager() {
  if (m_schedulerTimer) {
    m_schedulerTimer->Stop();
    delete m_schedulerTimer;
  }

  // Save all downloads to database before exiting
  SaveAllDownloadsToDatabase();

  // Cancel all active downloads
  CancelAllDownloads();
}

void DownloadManager::EnsureCategoryFoldersExist() {
  // Create main downloads folder
  CreateDirectoryA(m_defaultSavePath.c_str(), NULL);

  // Create category subfolders
  std::vector<std::string> categories = {"Compressed", "Documents", "Music",
                                         "Programs", "Video"};

  for (const auto &category : categories) {
    std::string folderPath = m_defaultSavePath + "\\" + category;
    CreateDirectoryA(folderPath.c_str(), NULL);
  }
}

void DownloadManager::LoadDownloadsFromDatabase() {
  DatabaseManager &db = DatabaseManager::GetInstance();
  db.Initialize();

  auto loadedDownloads = db.LoadAllDownloads();

  std::lock_guard<std::mutex> lock(m_downloadsMutex);

  for (auto &download : loadedDownloads) {
    // Track highest ID for new downloads
    if (download->GetId() >= m_nextId) {
      m_nextId = download->GetId() + 1;
    }

    // Convert unique_ptr to shared_ptr and add to list
    m_downloads.push_back(std::shared_ptr<Download>(download.release()));
  }
}

void DownloadManager::SaveAllDownloadsToDatabase() {
  DatabaseManager &db = DatabaseManager::GetInstance();

  std::lock_guard<std::mutex> lock(m_downloadsMutex);

  // Use transaction for batch save - all succeed or all fail
  if (!db.BeginTransaction()) {
    std::cerr << "Database error: Failed to begin transaction" << std::endl;
    return;
  }

  int failedCount = 0;
  for (const auto &download : m_downloads) {
    if (!db.SaveDownload(*download)) {
      failedCount++;
      std::cerr << "Database error: Failed to save download ID "
                << download->GetId() << " (" << download->GetFilename() << ")"
                << std::endl;
    }
  }

  if (failedCount > 0) {
    std::cerr << "Database warning: " << failedCount
              << " download(s) failed to save, rolling back" << std::endl;
    db.RollbackTransaction();
  } else {
    db.CommitTransaction();
  }
}

void DownloadManager::SaveDownloadToDatabase(int downloadId) {
  auto download = GetDownload(downloadId);
  if (download) {
    DatabaseManager::GetInstance().SaveDownload(*download);
  }
}

int DownloadManager::AddDownload(const std::string &url,
                                 const std::string &savePath) {
  // Validate URL first
  if (!IsValidUrl(url)) {
    return -1; // Invalid URL
  }

  std::lock_guard<std::mutex> lock(m_downloadsMutex);

  // Create download with default path first to get auto-detected category
  auto download =
      std::make_shared<Download>(m_nextId++, url, m_defaultSavePath);

  // Determine the correct folder based on category
  std::string category = download->GetCategory();

  if (!savePath.empty()) {
    download->SetSavePath(savePath); // User specified a custom path
  } else if (category != "All Downloads") {
    // Use category subfolder
    download->SetSavePath(m_defaultSavePath + "\\" + category);
  }
  // else: keep default save path

  m_downloads.push_back(download);

  // Save to database immediately
  if (!DatabaseManager::GetInstance().SaveDownload(*download)) {
    std::cerr << "Database error: Failed to save new download ID "
              << download->GetId() << std::endl;
  }

  return download->GetId();
}

void DownloadManager::RemoveDownload(int downloadId, bool deleteFile) {
  std::lock_guard<std::mutex> lock(m_downloadsMutex);

  auto it = std::find_if(m_downloads.begin(), m_downloads.end(),
                         [downloadId](const std::shared_ptr<Download> &d) {
                           return d->GetId() == downloadId;
                         });

  if (it != m_downloads.end()) {
    // Cancel if still downloading
    if ((*it)->GetStatus() == DownloadStatus::Downloading) {
      m_engine->CancelDownload(*it);
    }

    // Delete file if requested
    if (deleteFile) {
      std::string filePath = (*it)->GetSavePath() + "\\" + (*it)->GetFilename();
      DeleteFileA(filePath.c_str());
    }

    // Remove from database
    DatabaseManager::GetInstance().DeleteDownload(downloadId);

    m_downloads.erase(it);
  }
}

void DownloadManager::StartDownload(int downloadId) {
  std::shared_ptr<Download> download;

  {
    std::lock_guard<std::mutex> lock(m_downloadsMutex);
    auto it = std::find_if(m_downloads.begin(), m_downloads.end(),
                           [downloadId](const std::shared_ptr<Download> &d) {
                             return d->GetId() == downloadId;
                           });

    if (it != m_downloads.end()) {
      download = *it;
    }
  }

  if (download) {
    m_engine->StartDownload(download);
  }
}

void DownloadManager::PauseDownload(int downloadId) {
  std::lock_guard<std::mutex> lock(m_downloadsMutex);

  auto it = std::find_if(m_downloads.begin(), m_downloads.end(),
                         [downloadId](const std::shared_ptr<Download> &d) {
                           return d->GetId() == downloadId;
                         });

  if (it != m_downloads.end()) {
    m_engine->PauseDownload(*it);
    // Save updated status
    DatabaseManager::GetInstance().UpdateDownload(**it);
  }
}

void DownloadManager::ResumeDownload(int downloadId) {
  std::shared_ptr<Download> download;

  {
    std::lock_guard<std::mutex> lock(m_downloadsMutex);
    auto it = std::find_if(m_downloads.begin(), m_downloads.end(),
                           [downloadId](const std::shared_ptr<Download> &d) {
                             return d->GetId() == downloadId;
                           });

    if (it != m_downloads.end()) {
      download = *it;
    }
  }

  if (download) {
    m_engine->ResumeDownload(download);
  }
}

void DownloadManager::CancelDownload(int downloadId) {
  std::lock_guard<std::mutex> lock(m_downloadsMutex);

  auto it = std::find_if(m_downloads.begin(), m_downloads.end(),
                         [downloadId](const std::shared_ptr<Download> &d) {
                           return d->GetId() == downloadId;
                         });

  if (it != m_downloads.end()) {
    m_engine->CancelDownload(*it);
    // Save updated status
    DatabaseManager::GetInstance().UpdateDownload(**it);
  }
}

void DownloadManager::StartAllDownloads() {
  std::vector<std::shared_ptr<Download>> toStart;

  {
    std::lock_guard<std::mutex> lock(m_downloadsMutex);
    for (const auto &download : m_downloads) {
      if (download->GetStatus() == DownloadStatus::Queued ||
          download->GetStatus() == DownloadStatus::Paused) {
        toStart.push_back(download);
      }
    }
  }

  for (const auto &download : toStart) {
    m_engine->StartDownload(download);
  }
}

void DownloadManager::PauseAllDownloads() {
  std::lock_guard<std::mutex> lock(m_downloadsMutex);

  for (const auto &download : m_downloads) {
    if (download->GetStatus() == DownloadStatus::Downloading) {
      m_engine->PauseDownload(download);
    }
  }
}

void DownloadManager::CancelAllDownloads() {
  std::lock_guard<std::mutex> lock(m_downloadsMutex);

  for (const auto &download : m_downloads) {
    if (download->GetStatus() == DownloadStatus::Downloading ||
        download->GetStatus() == DownloadStatus::Paused) {
      m_engine->CancelDownload(download);
    }
  }
}

std::shared_ptr<Download> DownloadManager::GetDownload(int downloadId) const {
  std::lock_guard<std::mutex> lock(m_downloadsMutex);

  auto it = std::find_if(m_downloads.begin(), m_downloads.end(),
                         [downloadId](const std::shared_ptr<Download> &d) {
                           return d->GetId() == downloadId;
                         });

  return (it != m_downloads.end()) ? *it : nullptr;
}

std::vector<std::shared_ptr<Download>>
DownloadManager::GetAllDownloads() const {
  std::lock_guard<std::mutex> lock(m_downloadsMutex);
  return m_downloads;
}

std::vector<std::shared_ptr<Download>>
DownloadManager::GetDownloadsByCategory(const std::string &category) const {
  std::lock_guard<std::mutex> lock(m_downloadsMutex);

  std::vector<std::shared_ptr<Download>> result;
  for (const auto &download : m_downloads) {
    if (category == "All Downloads" || download->GetCategory() == category) {
      result.push_back(download);
    }
  }

  return result;
}

std::vector<std::shared_ptr<Download>>
DownloadManager::GetDownloadsByStatus(DownloadStatus status) const {
  std::lock_guard<std::mutex> lock(m_downloadsMutex);

  std::vector<std::shared_ptr<Download>> result;
  for (const auto &download : m_downloads) {
    if (download->GetStatus() == status) {
      result.push_back(download);
    }
  }

  return result;
}

int DownloadManager::GetTotalDownloads() const {
  std::lock_guard<std::mutex> lock(m_downloadsMutex);
  return static_cast<int>(m_downloads.size());
}

int DownloadManager::GetActiveDownloads() const {
  std::lock_guard<std::mutex> lock(m_downloadsMutex);

  int count = 0;
  for (const auto &download : m_downloads) {
    if (download->GetStatus() == DownloadStatus::Downloading) {
      count++;
    }
  }

  return count;
}

double DownloadManager::GetTotalSpeed() const {
  std::lock_guard<std::mutex> lock(m_downloadsMutex);

  double totalSpeed = 0.0;
  for (const auto &download : m_downloads) {
    if (download->GetStatus() == DownloadStatus::Downloading) {
      totalSpeed += download->GetSpeed();
    }
  }

  return totalSpeed;
}

void DownloadManager::OnDownloadProgress(int downloadId, int64_t downloaded,
                                         int64_t total, double speed) {
  if (m_updateCallback) {
    m_updateCallback(downloadId);
  }
}

void DownloadManager::OnDownloadComplete(int downloadId, bool success,
                                         const std::string &error) {
  // Save completed download to database
  auto download = GetDownload(downloadId);
  if (download) {
    DatabaseManager::GetInstance().UpdateDownload(*download);
  }

  if (m_updateCallback) {
    m_updateCallback(downloadId);
  }

  // If queue is running, check if we can start more
  if (m_isQueueRunning && success) {
    ProcessQueue();
  }
}

// Queue management
void DownloadManager::StartQueue() {
  m_isQueueRunning = true;
  ProcessQueue();
}

void DownloadManager::StopQueue() {
  m_isQueueRunning = false;
  // Note: We don't necessarily stop active downloads, just stop starting new
  // ones
}

void DownloadManager::ProcessQueue() {
  if (!m_isQueueRunning)
    return;

  int activeCount = GetActiveDownloads();
  if (activeCount >= m_maxSimultaneousDownloads)
    return;

  // Find queued downloads to start
  std::lock_guard<std::mutex> lock(m_downloadsMutex);

  for (auto &download : m_downloads) {
    if (activeCount >= m_maxSimultaneousDownloads)
      break;

    if (download->GetStatus() == DownloadStatus::Queued) {
      m_engine->StartDownload(download);
      activeCount++;
    }
  }
}

// Scheduling
void DownloadManager::SetSchedule(bool enableStart, const wxDateTime &startTime,
                                  bool enableStop, const wxDateTime &stopTime,
                                  int maxConcurrent, bool hangUp, bool exitApp,
                                  bool shutdown) {
  m_schedStartEnabled = enableStart;
  m_schedStartTime = startTime;
  m_schedStopEnabled = enableStop;
  m_schedStopTime = stopTime;
  m_maxSimultaneousDownloads = maxConcurrent;
  m_schedHangUp = hangUp;
  m_schedExit = exitApp;
  m_schedShutdown = shutdown;
}

void DownloadManager::CheckSchedule() {
  wxDateTime now = wxDateTime::Now();

  // Only check hours/minutes/seconds, ignore date part for daily schedule
  // Or actually, if we want specific date/time, we need to compare fully.
  // Let's match SchedulerDialog which gives full DateTime but user might expect
  // daily For now, assume exact time match within 1 second grace

  if (m_schedStartEnabled && !m_isQueueRunning) {
    if (now.GetHour() == m_schedStartTime.GetHour() &&
        now.GetMinute() == m_schedStartTime.GetMinute() &&
        now.GetSecond() == m_schedStartTime.GetSecond()) {
      StartQueue();
    }
  }

  if (m_schedStopEnabled && m_isQueueRunning) {
    if (now.GetHour() == m_schedStopTime.GetHour() &&
        now.GetMinute() == m_schedStopTime.GetMinute() &&
        now.GetSecond() == m_schedStopTime.GetSecond()) {
      StopQueue();

      // Handle completion actions
      if (m_schedExit) {
        wxExit();
      }
      // Todo: HangUp/Shutdown (require system calls)
    }
  }
}

void DownloadManager::OnSchedulerTimer(wxTimerEvent &event) {
  CheckSchedule();
  if (m_isQueueRunning) {
    ProcessQueue();
  }
}
