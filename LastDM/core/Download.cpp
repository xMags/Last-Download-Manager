#include "Download.h"
#include <algorithm>
#include <ctime>
#include <iomanip>
#include <sstream>

Download::Download(int id, const std::string &url, const std::string &savePath)
    : m_id(id), m_url(url), m_savePath(savePath), m_totalSize(-1),
      m_downloadedSize(0), m_status(DownloadStatus::Queued), m_speed(0.0) {
  m_filename = ExtractFilenameFromUrl(url);
  m_category = DetermineCategory(m_filename);
  UpdateLastTryTime();
}

std::string Download::GetFilename() const {
  std::lock_guard<std::mutex> lock(m_metadataMutex);
  return m_filename;
}

std::string Download::GetSavePath() const {
  std::lock_guard<std::mutex> lock(m_metadataMutex);
  return m_savePath;
}

std::string Download::GetStatusString() const {
  switch (m_status.load()) {
  case DownloadStatus::Queued:
    return "Queued";
  case DownloadStatus::Downloading:
    return "Downloading";
  case DownloadStatus::Paused:
    return "Paused";
  case DownloadStatus::Completed:
    return "Completed";
  case DownloadStatus::Error:
    return "Error";
  case DownloadStatus::Cancelled:
    return "Cancelled";
  default:
    return "Unknown";
  }
}

std::string Download::GetCategory() const {
  std::lock_guard<std::mutex> lock(m_metadataMutex);
  return m_category;
}

std::string Download::GetDescription() const {
  std::lock_guard<std::mutex> lock(m_metadataMutex);
  return m_description;
}

double Download::GetProgress() const {
  if (m_totalSize <= 0)
    return 0.0;
  return static_cast<double>(m_downloadedSize.load()) / m_totalSize * 100.0;
}

int Download::GetTimeRemaining() const {
  double speed = m_speed.load();
  if (speed <= 0 || m_totalSize <= 0)
    return -1;

  int64_t remaining = m_totalSize - m_downloadedSize.load();
  if (remaining <= 0)
    return 0;

  return static_cast<int>(remaining / speed);
}

std::string Download::GetLastTryTime() const {
  std::lock_guard<std::mutex> lock(m_metadataMutex);
  return m_lastTryTime;
}

std::string Download::GetErrorMessage() const {
  std::lock_guard<std::mutex> lock(m_metadataMutex);
  return m_errorMessage;
}

std::string Download::GetExpectedChecksum() const {
  std::lock_guard<std::mutex> lock(m_metadataMutex);
  return m_expectedChecksum;
}

std::string Download::GetCalculatedChecksum() const {
  std::lock_guard<std::mutex> lock(m_metadataMutex);
  return m_calculatedChecksum;
}

void Download::SetFilename(const std::string &filename) {
  std::lock_guard<std::mutex> lock(m_metadataMutex);
  m_filename = filename;
}

void Download::SetCategory(const std::string &category) {
  std::lock_guard<std::mutex> lock(m_metadataMutex);
  m_category = category;
}

void Download::SetDescription(const std::string &desc) {
  std::lock_guard<std::mutex> lock(m_metadataMutex);
  m_description = desc;
}

void Download::SetErrorMessage(const std::string &msg) {
  std::lock_guard<std::mutex> lock(m_metadataMutex);
  m_errorMessage = msg;
}

void Download::SetSavePath(const std::string &path) {
  std::lock_guard<std::mutex> lock(m_metadataMutex);
  m_savePath = path;
}

void Download::UpdateLastTryTime() {
  auto now = std::chrono::system_clock::now();
  std::time_t time = std::chrono::system_clock::to_time_t(now);
  std::tm tm;
  localtime_s(&tm, &time);

  std::stringstream ss;
  ss << std::put_time(&tm, "%Y-%m-%d %H:%M");
  {
    std::lock_guard<std::mutex> lock(m_metadataMutex);
    m_lastTryTime = ss.str();
  }
}

void Download::SetExpectedChecksum(const std::string &hash, int type) {
  std::lock_guard<std::mutex> lock(m_metadataMutex);
  m_expectedChecksum = hash;
  m_checksumType = type;
}

void Download::SetCalculatedChecksum(const std::string &hash) {
  std::lock_guard<std::mutex> lock(m_metadataMutex);
  m_calculatedChecksum = hash;
}

void Download::InitializeChunks(int numConnections) {
  std::lock_guard<std::mutex> lock(m_chunksMutex);
  m_chunks.clear();

  if (m_totalSize <= 0 || numConnections <= 1) {
    // Single chunk for unknown size or single connection
    m_chunks.emplace_back(0, m_totalSize > 0 ? m_totalSize - 1 : INT64_MAX);
    return;
  }

  int64_t chunkSize = m_totalSize / numConnections;
  int64_t startByte = 0;

  for (int i = 0; i < numConnections; ++i) {
    int64_t endByte =
        (i == numConnections - 1) ? m_totalSize - 1 : startByte + chunkSize - 1;
    m_chunks.emplace_back(startByte, endByte);
    startByte = endByte + 1;
  }
}

void Download::UpdateChunkProgress(int chunkIndex, int64_t currentByte) {
  std::lock_guard<std::mutex> lock(m_chunksMutex);

  if (chunkIndex >= 0 && chunkIndex < static_cast<int>(m_chunks.size())) {
    m_chunks[chunkIndex].currentByte = currentByte;
    if (currentByte >= m_chunks[chunkIndex].endByte) {
      m_chunks[chunkIndex].completed = true;
    }
  }

  RecalculateProgress();
}

void Download::RecalculateProgress() {
  int64_t totalDownloaded = 0;

  for (const auto &chunk : m_chunks) {
    totalDownloaded += chunk.currentByte - chunk.startByte;
  }

  m_downloadedSize = totalDownloaded;
}

std::string Download::ExtractFilenameFromUrl(const std::string &url) const {
  // Find the last part of the URL after the last '/'
  size_t lastSlash = url.rfind('/');
  if (lastSlash != std::string::npos && lastSlash < url.length() - 1) {
    std::string filename = url.substr(lastSlash + 1);

    // Remove query parameters
    size_t queryPos = filename.find('?');
    if (queryPos != std::string::npos) {
      filename = filename.substr(0, queryPos);
    }

    // URL decode common characters
    std::string decoded;
    for (size_t i = 0; i < filename.length(); ++i) {
      if (filename[i] == '%' && i + 2 < filename.length()) {
        int value;
        std::istringstream iss(filename.substr(i + 1, 2));
        if (iss >> std::hex >> value) {
          decoded += static_cast<char>(value);
          i += 2;
          continue;
        }
      }
      decoded += filename[i];
    }

    if (!decoded.empty()) {
      return decoded;
    }
  }

  return "download_" + std::to_string(m_id);
}

std::string Download::DetermineCategory(const std::string &filename) const {
  // Extract extension
  size_t dotPos = filename.rfind('.');
  if (dotPos == std::string::npos) {
    return "All Downloads";
  }

  std::string ext = filename.substr(dotPos + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

  // Compressed files
  if (ext == "zip" || ext == "rar" || ext == "7z" || ext == "tar" ||
      ext == "gz" || ext == "bz2") {
    return "Compressed";
  }

  // Documents
  if (ext == "pdf" || ext == "doc" || ext == "docx" || ext == "txt" ||
      ext == "xls" || ext == "xlsx" || ext == "ppt" || ext == "pptx") {
    return "Documents";
  }

  // Music
  if (ext == "mp3" || ext == "wav" || ext == "flac" || ext == "aac" ||
      ext == "ogg" || ext == "wma" || ext == "m4a") {
    return "Music";
  }

  // Video
  if (ext == "mp4" || ext == "avi" || ext == "mkv" || ext == "mov" ||
      ext == "wmv" || ext == "flv" || ext == "webm" || ext == "m4v") {
    return "Video";
  }

  // Images
  if (ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "gif" ||
      ext == "bmp" || ext == "webp" || ext == "svg" || ext == "ico" ||
      ext == "tiff" || ext == "tif") {
    return "Images";
  }

  // Programs
  if (ext == "exe" || ext == "msi" || ext == "dmg" || ext == "deb" ||
      ext == "rpm" || ext == "apk") {
    return "Programs";
  }

  return "All Downloads";
}

// Retry support methods for exponential backoff
bool Download::ShouldRetry() const {
  // Only retry if we have attempts remaining and status is Error
  return m_retryCount < m_maxRetries &&
         m_status.load() == DownloadStatus::Error;
}

int Download::GetRetryDelayMs() const {
  // Exponential backoff: 1s, 2s, 4s, 8s, 16s, ...
  // Formula: baseDelay * 2^retryCount
  constexpr int BASE_DELAY_MS = 1000;
  int delay = BASE_DELAY_MS * (1 << m_retryCount); // 2^retryCount

  // Cap at 60 seconds maximum
  constexpr int MAX_DELAY_MS = 60000;
  return std::min(delay, MAX_DELAY_MS);
}

void Download::IncrementRetry() {
  m_retryCount++;

  // Calculate and set next retry time
  int delayMs = GetRetryDelayMs();
  m_nextRetryTime =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
}

void Download::ResetRetry() {
  m_retryCount = 0;
  m_nextRetryTime = std::chrono::steady_clock::time_point{};
}
