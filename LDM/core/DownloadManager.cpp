#include "DownloadManager.h"
#include "YtDlpManager.h"
#include "../database/DatabaseManager.h"
#include "../utils/Settings.h"
#include <KnownFolders.h>
#include <Shlobj.h>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <unordered_set>
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

  // Reject blob: and data: URLs (sometimes passed with http prefix stripped incorrectly)
  if (url.find("blob:") != std::string::npos ||
      url.find("data:") != std::string::npos) {
    return false;
  }

  // Reject streaming manifest URLs
  if (url.find(".m3u8") != std::string::npos ||
      url.find(".mpd") != std::string::npos) {
    return false;
  }

  // Check URL length (WinINet has limits)
  if (url.length() > 2048) {
    return false;
  }

  // Find the host part - must exist after protocol
  size_t protocolEnd = url.find("://");
  if (protocolEnd == std::string::npos || protocolEnd + 3 >= url.length()) {
    return false;
  }

  // Check for a valid host (must have at least one character before / or end)
  size_t hostStart = protocolEnd + 3;
  size_t hostEnd = url.find('/', hostStart);
  if (hostEnd == std::string::npos) {
    hostEnd = url.length();
  }

  std::string host = url.substr(hostStart, hostEnd - hostStart);

  // Remove port if present
  size_t portPos = host.find(':');
  if (portPos != std::string::npos) {
    host = host.substr(0, portPos);
  }

  // Host must not be empty
  if (host.empty()) {
    return false;
  }

  // Host must contain at least one dot (except localhost)
  if (host != "localhost" && host != "127.0.0.1" && host.find('.') == std::string::npos) {
    return false;
  }

  return true;
}

static std::unordered_set<std::string> ParseExtensions(const std::string &csv) {
  std::unordered_set<std::string> result;
  std::istringstream stream(csv);
  std::string item;
  while (std::getline(stream, item, ',')) {
    size_t start = item.find_first_not_of(" \t\r\n");
    size_t end = item.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) {
      continue;
    }
    std::string ext = item.substr(start, end - start + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (!ext.empty()) {
      result.insert(ext);
    }
  }
  return result;
}

static std::string DetermineCategoryFromSettings(const std::string &filename) {
  size_t dotPos = filename.rfind('.');
  if (dotPos == std::string::npos) {
    return "All Downloads";
  }

  std::string ext = filename.substr(dotPos + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

  DatabaseManager &db = DatabaseManager::GetInstance();

  auto compressed = ParseExtensions(db.GetSetting("file_types_compressed", "zip,rar,7z,tar,gz"));
  if (compressed.find(ext) != compressed.end()) {
    return "Compressed";
  }

  auto documents = ParseExtensions(db.GetSetting("file_types_documents", "pdf,doc,docx,txt,xls,xlsx,ppt,pptx"));
  if (documents.find(ext) != documents.end()) {
    return "Documents";
  }

  auto images = ParseExtensions(db.GetSetting("file_types_images", "jpg,jpeg,png,gif,bmp,webp,svg,ico,tiff,tif"));
  if (images.find(ext) != images.end()) {
    return "Images";
  }

  auto music = ParseExtensions(db.GetSetting("file_types_music", "mp3,wav,flac,aac,ogg,wma"));
  if (music.find(ext) != music.end()) {
    return "Music";
  }

  auto video = ParseExtensions(db.GetSetting("file_types_video", "mp4,avi,mkv,mov,wmv,flv,webm"));
  if (video.find(ext) != video.end()) {
    return "Video";
  }

  auto programs = ParseExtensions(db.GetSetting("file_types_programs", "exe,msi,dmg,deb,rpm,apk"));
  if (programs.find(ext) != programs.end()) {
    return "Programs";
  }

  return "All Downloads";
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
  m_engine->SetProgressCallback([this](int downloadId, int64_t downloaded,
                                       int64_t total, double speed) {
    OnDownloadProgress(downloadId, downloaded, total, speed);
  });
  m_engine->SetCompletionCallback(
      [this](int downloadId, bool success, const std::string &error) {
        OnDownloadComplete(downloadId, success, error);
      });

  // Set default save path to Downloads folder
  PWSTR path = NULL;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, NULL, &path))) {
    // Convert to std::string (ANSI)
    int len =
        WideCharToMultiByte(CP_UTF8, 0, path, -1, NULL, 0, NULL, NULL);
    if (len > 0) {
      std::vector<char> buf(len);
      WideCharToMultiByte(CP_UTF8, 0, path, -1, buf.data(), len, NULL, NULL);
      m_defaultSavePath = buf.data();
    }
    CoTaskMemFree(path);
  } else {
    // Fallback to Profile/Downloads
    char pathA[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PROFILE, NULL, 0, pathA))) {
      m_defaultSavePath = std::string(pathA) + "\\Downloads";
    } else {
      m_defaultSavePath = "C:\\Downloads";
    }
  }

  // Load downloads from database
  LoadDownloadsFromDatabase();

  // Now that database is initialized, load settings
  Settings::GetInstance().Load();
  ApplySettings(Settings::GetInstance());

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
                                         "Programs", "Video", "Images"};

  for (const auto &category : categories) {
    std::string folderPath = m_defaultSavePath + "\\" + category;
    CreateDirectoryA(folderPath.c_str(), NULL);
  }
}

void DownloadManager::ApplySettings(const Settings &settings) {
  // Use user's download folder setting if valid, otherwise keep default
  wxString userFolder = settings.GetDownloadFolder();
  if (!userFolder.IsEmpty()) {
    m_defaultSavePath = userFolder.ToStdString();
  }

  m_maxSimultaneousDownloads = settings.GetMaxSimultaneousDownloads();
  EnsureCategoryFoldersExist();

  if (m_engine) {
    m_engine->SetMaxConnections(std::max(1, settings.GetMaxConnections()));

    int speedLimitKb = settings.GetSpeedLimit();
    int64_t speedLimitBytes =
        speedLimitKb > 0 ? static_cast<int64_t>(speedLimitKb) * 1024 : 0;
    m_engine->SetSpeedLimit(speedLimitBytes);

    if (settings.GetUseProxy()) {
      m_engine->SetProxy(settings.GetProxyHost(), settings.GetProxyPort());
    } else {
      m_engine->SetProxy("", 0);
    }
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
    auto sharedDownload = std::shared_ptr<Download>(download.release());
    m_downloads.push_back(sharedDownload);
    m_downloadIndex[sharedDownload->GetId()] = sharedDownload;
  }
}

void DownloadManager::SaveAllDownloadsToDatabase() {
  DatabaseManager &db = DatabaseManager::GetInstance();

  std::lock_guard<std::mutex> lock(m_downloadsMutex);
  if (!db.SyncAllDownloads(m_downloads)) {
    std::cerr << "Database error: Failed to save downloads" << std::endl;
  }
}

void DownloadManager::UpdateDownloadsCategory(const std::string &fromCategory,
                                              const std::string &toCategory) {
  std::vector<std::shared_ptr<Download>> toUpdate;
  {
    std::lock_guard<std::mutex> lock(m_downloadsMutex);
    for (const auto &download : m_downloads) {
      if (download && download->GetCategory() == fromCategory) {
        download->SetCategory(toCategory);
        toUpdate.push_back(download);
      }
    }
  }

  if (toUpdate.empty()) {
    return;
  }

  DatabaseManager &db = DatabaseManager::GetInstance();
  for (const auto &download : toUpdate) {
    db.UpdateDownload(*download);
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

  // Check if this is a video site URL
  YtDlpManager &ytdlp = YtDlpManager::GetInstance();
  bool isVideoSite = ytdlp.IsVideoSiteUrl(url);

  // Determine the correct folder based on category
  std::string category = DetermineCategoryFromSettings(download->GetFilename());
  download->SetCategory(category);

  if (!savePath.empty()) {
    download->SetSavePath(savePath); // User specified a custom path
  } else if (isVideoSite) {
    // Video site URLs always go to Video folder
    download->SetSavePath(m_defaultSavePath + "\\Video");
    download->SetCategory("Video");
    download->SetYtDlpDownload(true);
  } else if (category != "All Downloads") {
    // Use category subfolder
    download->SetSavePath(m_defaultSavePath + "\\" + category);
  }
  // else: keep default save path

  m_downloads.push_back(download);
  m_downloadIndex[download->GetId()] = download;

  // Save to database immediately
  if (!DatabaseManager::GetInstance().SaveDownload(*download)) {
    std::cerr << "Database error: Failed to save new download ID "
              << download->GetId() << std::endl;
  }

  return download->GetId();
}

void DownloadManager::RemoveDownload(int downloadId, bool deleteFile) {
  std::shared_ptr<Download> downloadToRemove;

  {
    std::lock_guard<std::mutex> lock(m_downloadsMutex);

    auto it = std::find_if(m_downloads.begin(), m_downloads.end(),
                           [downloadId](const std::shared_ptr<Download> &d) {
                             return d->GetId() == downloadId;
                           });

    if (it == m_downloads.end()) {
      return;
    }

    downloadToRemove = *it;

    // Cancel if still downloading
    if (downloadToRemove->GetStatus() == DownloadStatus::Downloading) {
      if (downloadToRemove->IsYtDlpDownload()) {
        YtDlpManager::GetInstance().CancelDownload(downloadId);
      } else {
        m_engine->CancelDownload(downloadToRemove);
      }
    }

    // Remove from index and list first
    m_downloadIndex.erase(downloadId);
    m_downloads.erase(it);
  }

  // Wait for download thread to finish (outside the lock to avoid deadlock)
  // This ensures file handles are released before we try to delete
  if (downloadToRemove->IsYtDlpDownload()) {
    YtDlpManager::GetInstance().WaitForDownloadFinish(downloadId, 5000);
  } else {
    m_engine->WaitForDownloadFinish(downloadId, 5000);
  }

  // Delete file if requested
  if (deleteFile) {
    std::string filePath = downloadToRemove->GetSavePath() + "\\" + downloadToRemove->GetFilename();
    DeleteFileA(filePath.c_str());

    // Also delete any .partN files that may exist
    auto chunks = downloadToRemove->GetChunksCopy();
    for (size_t i = 0; i < chunks.size(); ++i) {
      std::string partPath = filePath + ".part" + std::to_string(i);
      DeleteFileA(partPath.c_str());
    }
  }

  // Remove from database
  DatabaseManager::GetInstance().DeleteDownload(downloadId);
}

void DownloadManager::StartDownload(int downloadId) {
  std::shared_ptr<Download> download;

  {
    std::lock_guard<std::mutex> lock(m_downloadsMutex);
    auto it = m_downloadIndex.find(downloadId);
    if (it != m_downloadIndex.end()) {
      download = it->second;
    }
  }

  if (download) {
    // Check if this is a video site URL that should use yt-dlp
    YtDlpManager &ytdlp = YtDlpManager::GetInstance();
    if (ytdlp.IsVideoSiteUrl(download->GetUrl())) {
      download->SetYtDlpDownload(true);
      download->SetCategory("Video");

      // Always set save path to Video subfolder for yt-dlp downloads
      // (Override the default path since video site URLs don't have proper extensions)
      download->SetSavePath(m_defaultSavePath + "\\Video");

      // Check if yt-dlp is available
      if (!ytdlp.IsYtDlpAvailable()) {
        // yt-dlp not installed - set error and let UI handle it
        download->SetStatus(DownloadStatus::Error);
        download->SetErrorMessage("yt-dlp not installed. Click to install.");
        return;
      }

      ytdlp.StartDownload(download);
    } else {
      m_engine->StartDownload(download);
    }
  }
}

void DownloadManager::StartDownloadWithFormat(int downloadId, const std::string &formatId) {
  std::shared_ptr<Download> download;

  {
    std::lock_guard<std::mutex> lock(m_downloadsMutex);
    auto it = m_downloadIndex.find(downloadId);
    if (it != m_downloadIndex.end()) {
      download = it->second;
    }
  }

  if (download) {
    // Check if this is a video site URL that should use yt-dlp
    YtDlpManager &ytdlp = YtDlpManager::GetInstance();
    if (ytdlp.IsVideoSiteUrl(download->GetUrl())) {
      download->SetYtDlpDownload(true);
      download->SetCategory("Video");

      // Always set save path to Video subfolder for yt-dlp downloads
      download->SetSavePath(m_defaultSavePath + "\\Video");

      // Check if yt-dlp is available
      if (!ytdlp.IsYtDlpAvailable()) {
        download->SetStatus(DownloadStatus::Error);
        download->SetErrorMessage("yt-dlp not installed. Click to install.");
        return;
      }

      ytdlp.StartDownloadWithFormat(download, formatId);
    } else {
      // Non-video files don't support format selection
      m_engine->StartDownload(download);
    }
  }
}

void DownloadManager::PauseDownload(int downloadId) {
  std::shared_ptr<Download> download;
  {
    std::lock_guard<std::mutex> lock(m_downloadsMutex);
    auto it = m_downloadIndex.find(downloadId);
    if (it != m_downloadIndex.end()) {
      download = it->second;
    }
  }

  if (download) {
    if (download->IsYtDlpDownload()) {
      YtDlpManager::GetInstance().PauseDownload(downloadId);
      download->SetStatus(DownloadStatus::Paused);
    } else {
      m_engine->PauseDownload(download);
    }
    // Save updated status
    DatabaseManager::GetInstance().UpdateDownload(*download);
  }
}

void DownloadManager::ResumeDownload(int downloadId) {
  std::shared_ptr<Download> download;

  {
    std::lock_guard<std::mutex> lock(m_downloadsMutex);
    auto it = m_downloadIndex.find(downloadId);
    if (it != m_downloadIndex.end()) {
      download = it->second;
    }
  }

  if (download) {
    if (download->IsYtDlpDownload()) {
      YtDlpManager::GetInstance().ResumeDownload(download);
    } else {
      m_engine->ResumeDownload(download);
    }
  }
}

void DownloadManager::CancelDownload(int downloadId) {
  std::shared_ptr<Download> download;
  {
    std::lock_guard<std::mutex> lock(m_downloadsMutex);
    auto it = m_downloadIndex.find(downloadId);
    if (it != m_downloadIndex.end()) {
      download = it->second;
    }
  }

  if (download) {
    if (download->IsYtDlpDownload()) {
      YtDlpManager::GetInstance().CancelDownload(downloadId);
      download->SetStatus(DownloadStatus::Cancelled);
    } else {
      m_engine->CancelDownload(download);
    }
    // Save updated status
    DatabaseManager::GetInstance().UpdateDownload(*download);
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
    if (download->IsYtDlpDownload()) {
      YtDlpManager::GetInstance().StartDownload(download);
    } else {
      m_engine->StartDownload(download);
    }
  }
}

void DownloadManager::PauseAllDownloads() {
  std::vector<std::shared_ptr<Download>> toPause;
  {
    std::lock_guard<std::mutex> lock(m_downloadsMutex);
    for (const auto &download : m_downloads) {
      if (download->GetStatus() == DownloadStatus::Downloading) {
        toPause.push_back(download);
      }
    }
  }

  for (const auto &download : toPause) {
    if (download->IsYtDlpDownload()) {
      YtDlpManager::GetInstance().PauseDownload(download->GetId());
      download->SetStatus(DownloadStatus::Paused);
    } else {
      m_engine->PauseDownload(download);
    }
  }
}

void DownloadManager::CancelAllDownloads() {
  std::vector<std::shared_ptr<Download>> toCancel;
  {
    std::lock_guard<std::mutex> lock(m_downloadsMutex);
    for (const auto &download : m_downloads) {
      if (download->GetStatus() == DownloadStatus::Downloading ||
          download->GetStatus() == DownloadStatus::Paused) {
        toCancel.push_back(download);
      }
    }
  }

  for (const auto &download : toCancel) {
    if (download->IsYtDlpDownload()) {
      YtDlpManager::GetInstance().CancelDownload(download->GetId());
      download->SetStatus(DownloadStatus::Cancelled);
    } else {
      m_engine->CancelDownload(download);
    }
  }
}

std::shared_ptr<Download> DownloadManager::GetDownload(int downloadId) const {
  std::lock_guard<std::mutex> lock(m_downloadsMutex);

  auto it = m_downloadIndex.find(downloadId);
  return (it != m_downloadIndex.end()) ? it->second : nullptr;
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
  result.reserve(m_downloads.size());  // Avoid reallocations
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
  result.reserve(m_downloads.size());  // Avoid reallocations
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
  DownloadUpdateCallback callback;
  {
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    callback = m_updateCallback;
  }
  if (callback) {
    callback(downloadId);
  }
}

void DownloadManager::OnDownloadComplete(int downloadId, bool success,
                                         const std::string &error) {
  // Save completed download to database
  auto download = GetDownload(downloadId);
  if (download) {
    DatabaseManager::GetInstance().UpdateDownload(*download);
  }

  DownloadUpdateCallback callback;
  {
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    callback = m_updateCallback;
  }
  if (callback) {
    callback(downloadId);
  }

  // If queue is running, check if we can start more
  // Always call ProcessQueue regardless of success to fill vacant slot
  if (m_isQueueRunning.load()) {
    ProcessQueue();
  }
}

// Queue management
void DownloadManager::StartQueue() {
  m_isQueueRunning.store(true);
  ProcessQueue();
}

void DownloadManager::StopQueue() {
  m_isQueueRunning.store(false);
  // Note: We don't necessarily stop active downloads, just stop starting new
  // ones
}

void DownloadManager::ProcessQueue() {
  if (!m_isQueueRunning.load())
    return;

  // Lock once to avoid TOCTOU race between GetActiveDownloads and starting downloads
  std::lock_guard<std::mutex> lock(m_downloadsMutex);

  // Count active downloads while holding the lock
  int activeCount = 0;
  for (const auto &download : m_downloads) {
    if (download->GetStatus() == DownloadStatus::Downloading) {
      activeCount++;
    }
  }

  if (activeCount >= m_maxSimultaneousDownloads)
    return;

  // Find queued downloads to start
  for (auto &download : m_downloads) {
    if (activeCount >= m_maxSimultaneousDownloads)
      break;

    if (download->GetStatus() == DownloadStatus::Queued) {
      // Use appropriate download method based on download type
      if (download->IsYtDlpDownload()) {
        YtDlpManager &ytdlp = YtDlpManager::GetInstance();
        if (ytdlp.IsYtDlpAvailable()) {
          ytdlp.StartDownload(download);
        } else {
          download->SetStatus(DownloadStatus::Error);
          download->SetErrorMessage("yt-dlp not installed. Click to install.");
        }
      } else {
        m_engine->StartDownload(download);
      }
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
  int currentMinute = now.GetHour() * 60 + now.GetMinute();

  // Start schedule - check if we should start the queue
  if (m_schedStartEnabled && !m_isQueueRunning.load()) {
    int schedStartMinute = m_schedStartTime.GetHour() * 60 + m_schedStartTime.GetMinute();
    // Only trigger once per minute (prevents double-trigger within same minute)
    if (currentMinute == schedStartMinute && m_lastSchedStartMinute != currentMinute) {
      m_lastSchedStartMinute = currentMinute;
      StartQueue();
    }
  }

  // Stop schedule - check if we should stop the queue
  if (m_schedStopEnabled && m_isQueueRunning.load()) {
    int schedStopMinute = m_schedStopTime.GetHour() * 60 + m_schedStopTime.GetMinute();
    // Only trigger once per minute (prevents double-trigger within same minute)
    if (currentMinute == schedStopMinute && m_lastSchedStopMinute != currentMinute) {
      m_lastSchedStopMinute = currentMinute;
      StopQueue();

      // Handle completion actions
      if (m_schedShutdown) {
        // Show warning and give user a chance to cancel
        int result = wxMessageBox(
            "LDM is about to shut down the computer as scheduled.\n\n"
            "Click Cancel within 30 seconds to abort.",
            "Scheduled Shutdown",
            wxOK | wxCANCEL | wxICON_WARNING);

        if (result == wxOK) {
          // Initiate system shutdown with proper privilege check
#ifdef _WIN32
          // Request shutdown privilege
          HANDLE hToken;
          TOKEN_PRIVILEGES tkp;
          if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
            LookupPrivilegeValue(NULL, SE_SHUTDOWN_NAME, &tkp.Privileges[0].Luid);
            tkp.PrivilegeCount = 1;
            tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(hToken, FALSE, &tkp, 0, NULL, 0);
            CloseHandle(hToken);
          }
          ExitWindowsEx(EWX_SHUTDOWN | EWX_FORCE, SHTDN_REASON_MAJOR_APPLICATION);
#endif
        }
      } else if (m_schedExit) {
        wxTheApp->Exit();
      }
    }
  }
}

void DownloadManager::OnSchedulerTimer(wxTimerEvent &event) {
  CheckSchedule();
  if (m_isQueueRunning.load()) {
    ProcessQueue();
  }
}
