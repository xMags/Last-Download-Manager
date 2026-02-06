#include "Download.h"
#include <algorithm>
#include <ctime>
#include <iomanip>
#include <limits>
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
  // If manual progress is set (yt-dlp), use it
  double manual = m_manualProgress.load();
  if (manual >= 0.0) {
    return manual;
  }

  int64_t totalSize = m_totalSize.load();
  if (totalSize <= 0)
    return 0.0;
  return static_cast<double>(m_downloadedSize.load()) / totalSize * 100.0;
}

int Download::GetTimeRemaining() const {
  double speed = m_speed.load();
  int64_t totalSize = m_totalSize.load();
  if (speed <= 0 || totalSize <= 0)
    return -1;

  int64_t remaining = totalSize - m_downloadedSize.load();
  if (remaining <= 0)
    return 0;

  double secondsRemaining = remaining / speed;
  if (secondsRemaining >= static_cast<double>(std::numeric_limits<int>::max())) {
    return std::numeric_limits<int>::max();
  }
  return static_cast<int>(secondsRemaining);
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

void Download::SetReferer(const std::string &referer) {
  std::lock_guard<std::mutex> lock(m_metadataMutex);
  m_referer = referer;
}

void Download::SetProgress(double progress) {
  m_manualProgress.store(progress);
}

void Download::SetCategory(const std::string &category) {
  std::lock_guard<std::mutex> lock(m_metadataMutex);
  m_category = category;
}

void Download::SetDescription(const std::string &desc) {
  std::lock_guard<std::mutex> lock(m_metadataMutex);
  m_description = desc;
}

void Download::SetSpeed(double speed) {
  // Use exponential moving average (EMA) for smooth speed display
  // Alpha controls smoothing: lower = smoother, higher = more responsive
  constexpr double ALPHA = 0.2;
  constexpr int WARMUP_SAMPLES = 3;

  // Use mutex to protect the read-modify-write sequence
  std::lock_guard<std::mutex> lock(m_metadataMutex);

  // Since we hold the mutex, we can use relaxed ordering - the mutex provides synchronization
  int sampleCount = m_speedSampleCount.load(std::memory_order_relaxed);

  if (sampleCount < WARMUP_SAMPLES) {
    // During warmup, use simple average to establish baseline
    double currentSmoothed = m_smoothedSpeed.load(std::memory_order_relaxed);
    double newSmoothed = (currentSmoothed * sampleCount + speed) / (sampleCount + 1);
    m_smoothedSpeed.store(newSmoothed, std::memory_order_relaxed);
    m_speedSampleCount.store(sampleCount + 1, std::memory_order_relaxed);
    m_speed.store(newSmoothed, std::memory_order_release);  // Release for readers
  } else {
    // Apply EMA: smoothed = alpha * new + (1 - alpha) * previous
    double previousSmoothed = m_smoothedSpeed.load(std::memory_order_relaxed);
    double newSmoothed = ALPHA * speed + (1.0 - ALPHA) * previousSmoothed;
    m_smoothedSpeed.store(newSmoothed, std::memory_order_relaxed);
    m_speed.store(newSmoothed, std::memory_order_release);  // Release for readers
  }
}

void Download::ResetSpeed() {
  std::lock_guard<std::mutex> lock(m_metadataMutex);
  m_speed.store(0.0, std::memory_order_relaxed);
  m_smoothedSpeed.store(0.0, std::memory_order_relaxed);
  m_speedSampleCount.store(0, std::memory_order_relaxed);
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
  std::tm tm = {};  // Initialize all fields to zero
  errno_t err = localtime_s(&tm, &time);

  std::stringstream ss;
  if (err == 0) {
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M");
  } else {
    // Fallback to timestamp if localtime_s fails
    ss << "Error-" << time;
  }
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

  int64_t totalSize = m_totalSize.load();
  if (totalSize <= 0 || numConnections <= 1) {
    // Single chunk for unknown size or single connection
    // Use -1 as sentinel for unknown end (streaming download)
    m_chunks.emplace_back(0, totalSize > 0 ? totalSize - 1 : -1);
    return;
  }

  int64_t chunkSize = totalSize / numConnections;
  int64_t startByte = 0;

  for (int i = 0; i < numConnections; ++i) {
    int64_t endByte = (i == numConnections - 1)
                          ? totalSize - 1
                          : startByte + chunkSize - 1;
    m_chunks.emplace_back(startByte, endByte);
    startByte = endByte + 1;
  }
}

void Download::UpdateChunkProgress(int chunkIndex, int64_t currentByte) {
  std::lock_guard<std::mutex> lock(m_chunksMutex);

  if (chunkIndex >= 0 && chunkIndex < static_cast<int>(m_chunks.size())) {
    m_chunks[chunkIndex].currentByte = currentByte;
    // endByte is inclusive, so completed when currentByte > endByte
    // endByte of -1 means unknown size (streaming), never auto-complete
    if (m_chunks[chunkIndex].endByte >= 0 && 
        currentByte > m_chunks[chunkIndex].endByte) {
      m_chunks[chunkIndex].completed = true;
    }
  }

  RecalculateProgress();
}

std::vector<DownloadChunk> Download::GetChunksCopy() const {
  std::lock_guard<std::mutex> lock(m_chunksMutex);
  return m_chunks;
}

void Download::SetChunks(const std::vector<DownloadChunk> &chunks) {
  std::lock_guard<std::mutex> lock(m_chunksMutex);
  m_chunks = chunks;
  RecalculateProgress();
}

void Download::RecalculateProgress() {
  int64_t totalDownloaded = 0;

  for (const auto &chunk : m_chunks) {
    // currentByte is the next byte to download, so downloaded = currentByte - startByte
    // But we need to handle the case where chunk hasn't started (currentByte == startByte)
    if (chunk.currentByte > chunk.startByte) {
      totalDownloaded += chunk.currentByte - chunk.startByte;
    }
    // If completed, ensure we count the full chunk
    if (chunk.completed && chunk.endByte >= 0) {
      int64_t chunkSize = chunk.endByte - chunk.startByte + 1;
      int64_t counted = chunk.currentByte - chunk.startByte;
      // If somehow we counted less than the full chunk but it's marked complete, fix it
      if (counted < chunkSize) {
        totalDownloaded += (chunkSize - counted);
      }
    }
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

    // Sanitize invalid Windows filename characters
    std::string sanitized;
    for (char c : decoded) {
      if (c == ':' || c == '*' || c == '?' || c == '"' ||
          c == '<' || c == '>' || c == '|' || c == '\\' || c == '/') {
        sanitized += '_';  // Replace invalid chars with underscore
      } else if (c >= 32) {  // Skip control characters
        sanitized += c;
      }
    }

    // Trim trailing dots and spaces (Windows doesn't allow these at end of filenames)
    while (!sanitized.empty() && (sanitized.back() == '.' || sanitized.back() == ' ')) {
      sanitized.pop_back();
    }

    // Also trim leading spaces
    size_t start = sanitized.find_first_not_of(' ');
    if (start != std::string::npos && start > 0) {
      sanitized = sanitized.substr(start);
    }

    if (!sanitized.empty()) {
      return sanitized;
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
  return m_retryCount.load() < m_maxRetries.load() &&
         m_status.load() == DownloadStatus::Error;
}

int Download::GetRetryDelayMs() const {
  // Exponential backoff: 1s, 2s, 4s, 8s, 16s, ...
  // Formula: baseDelay * 2^retryCount
  constexpr int BASE_DELAY_MS = 1000;
  int retryCount = m_retryCount.load();
  // Cap at 5 to prevent integer overflow (1 << 5 = 32)
  int cappedRetry = std::min(retryCount, 5);
  int delay = BASE_DELAY_MS * (1 << cappedRetry); // 2^retryCount

  // Cap at 60 seconds maximum
  constexpr int MAX_DELAY_MS = 60000;
  return std::min(delay, MAX_DELAY_MS);
}

void Download::IncrementRetry() {
  int currentRetry = m_retryCount.fetch_add(1);

  // Calculate and set next retry time
  // Use currentRetry+1 since we just incremented
  constexpr int BASE_DELAY_MS = 1000;
  int cappedRetry = std::min(currentRetry + 1, 5);
  int delayMs = BASE_DELAY_MS * (1 << cappedRetry);
  delayMs = std::min(delayMs, 60000);
  
  std::lock_guard<std::mutex> lock(m_metadataMutex);
  m_nextRetryTime =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
}

void Download::ResetRetry() {
  m_retryCount.store(0);
  std::lock_guard<std::mutex> lock(m_metadataMutex);
  m_nextRetryTime = std::chrono::steady_clock::time_point{};
}
