#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Forward declaration for hash types
enum class HashType;

enum class DownloadStatus {
  Queued,
  Downloading,
  Paused,
  Completed,
  Error,
  Cancelled
};

struct DownloadChunk {
  int64_t startByte;
  int64_t endByte;
  int64_t currentByte;
  bool completed;

  DownloadChunk(int64_t start, int64_t end)
      : startByte(start), endByte(end), currentByte(start), completed(false) {}

  double GetProgress() const {
    if (completed) return 100.0;
    if (endByte < 0) return 0.0;  // Unknown size
    int64_t total = endByte - startByte + 1;  // +1 because endByte is inclusive
    if (total <= 0) return 0.0;
    int64_t downloaded = currentByte - startByte;
    if (downloaded <= 0) return 0.0;
    if (downloaded >= total) return 100.0;
    return static_cast<double>(downloaded) / total * 100.0;
  }
};

class Download {
public:
  Download(int id, const std::string &url, const std::string &savePath);
  ~Download() = default;

  // Getters
  int GetId() const { return m_id; }
  std::string GetUrl() const {
    std::lock_guard<std::mutex> lock(m_metadataMutex);
    return m_url;
  }
  std::string GetReferer() const {
    std::lock_guard<std::mutex> lock(m_metadataMutex);
    return m_referer;
  }
  std::string GetFilename() const;
  std::string GetSavePath() const;
  int64_t GetTotalSize() const { return m_totalSize.load(); }
  int64_t GetDownloadedSize() const { return m_downloadedSize.load(); }
  DownloadStatus GetStatus() const { return m_status.load(); }
  std::string GetStatusString() const;
  std::string GetCategory() const;
  std::string GetDescription() const;
  double GetProgress() const;
  double GetSpeed() const { return m_speed.load(); }
  int GetTimeRemaining() const;
  std::string GetLastTryTime() const;
  std::string GetErrorMessage() const;

  // Retry support
  int GetRetryCount() const { return m_retryCount.load(); }
  int GetMaxRetries() const { return m_maxRetries.load(); }
  bool ShouldRetry() const;
  std::chrono::steady_clock::time_point GetNextRetryTime() const {
    std::lock_guard<std::mutex> lock(m_metadataMutex);
    return m_nextRetryTime;
  }
  int GetRetryDelayMs() const; // Get current delay in milliseconds

  // Checksum support
  std::string GetExpectedChecksum() const;
  std::string GetCalculatedChecksum() const;
  int GetChecksumType() const {
    std::lock_guard<std::mutex> lock(m_metadataMutex);
    return m_checksumType;
  } // 0=None, 1=MD5, 2=SHA256
  bool IsChecksumVerified() const { 
    std::lock_guard<std::mutex> lock(m_metadataMutex);
    return m_checksumVerified; 
  }

  // Setters
  void SetFilename(const std::string &filename);
  void SetReferer(const std::string &referer);
  void SetTotalSize(int64_t size) { m_totalSize.store(size); }
  void SetDownloadedSize(int64_t size) { m_downloadedSize.store(size); }
  void SetStatus(DownloadStatus status) { m_status.store(status); }
  void SetCategory(const std::string &category);
  void SetDescription(const std::string &desc);
  void SetSpeed(double speed);
  void ResetSpeed();
  void SetErrorMessage(const std::string &msg);
  void SetSavePath(const std::string &path);
  void UpdateLastTryTime();
  void SetProgress(double progress);  // For yt-dlp downloads

  // yt-dlp download flag
  bool IsYtDlpDownload() const { return m_isYtDlpDownload.load(); }
  void SetYtDlpDownload(bool isYtDlp) { m_isYtDlpDownload.store(isYtDlp); }

  // Retry support
  void SetMaxRetries(int maxRetries) { m_maxRetries.store(maxRetries); }
  void IncrementRetry(); // Increment retry count and calculate next retry time
  void ResetRetry();     // Reset retry count (on success or user restart)

  // Checksum support
  void SetExpectedChecksum(const std::string &hash, int type);
  void SetCalculatedChecksum(const std::string &hash);
  void SetChecksumVerified(bool verified) { 
    std::lock_guard<std::mutex> lock(m_metadataMutex);
    m_checksumVerified = verified; 
  }

  // Chunk management
  void InitializeChunks(int numConnections);
  std::vector<DownloadChunk> GetChunksCopy() const;
  void SetChunks(const std::vector<DownloadChunk> &chunks);
  void UpdateChunkProgress(int chunkIndex, int64_t currentByte);

  // Progress calculation
  void RecalculateProgress();

private:
  int m_id;
  std::string m_url;
  std::string m_referer;  // Page URL for protected downloads
  std::string m_filename;
  std::string m_savePath;
  std::atomic<int64_t> m_totalSize;
  std::atomic<int64_t> m_downloadedSize;
  std::atomic<DownloadStatus> m_status;
  std::string m_category;
  std::string m_description;
  std::atomic<double> m_speed;
  std::atomic<double> m_smoothedSpeed{0.0};  // EMA smoothed speed
  std::atomic<int> m_speedSampleCount{0};    // Sample count for initial ramp-up
  std::atomic<bool> m_isYtDlpDownload{false}; // True if handled by yt-dlp
  std::atomic<double> m_manualProgress{-1.0}; // Manual progress for yt-dlp (-1 = use calculated)
  std::string m_lastTryTime;
  std::string m_errorMessage;

  // Retry tracking for exponential backoff
  std::atomic<int> m_retryCount{0}; // Current retry attempt (0 = first try)
  std::atomic<int> m_maxRetries{5}; // Maximum retry attempts
  std::chrono::steady_clock::time_point m_nextRetryTime; // When to retry next (protected by m_metadataMutex)

  // Checksum verification
  std::string m_expectedChecksum;   // User-provided expected hash
  std::string m_calculatedChecksum; // Calculated hash after download
  int m_checksumType = 0;           // 0=None, 1=MD5, 2=SHA256
  bool m_checksumVerified = false;  // Was checksum verified successfully?

  std::vector<DownloadChunk> m_chunks;
  mutable std::mutex m_chunksMutex;
  mutable std::mutex m_metadataMutex;

  std::string ExtractFilenameFromUrl(const std::string &url) const;
  std::string DetermineCategory(const std::string &filename) const;
};
