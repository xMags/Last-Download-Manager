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
    if (endByte <= startByte)
      return 100.0;
    return static_cast<double>(currentByte - startByte) /
           (endByte - startByte) * 100.0;
  }
};

class Download {
public:
  Download(int id, const std::string &url, const std::string &savePath);
  ~Download() = default;

  // Getters
  int GetId() const { return m_id; }
  std::string GetUrl() const { return m_url; }
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
  int GetRetryCount() const { return m_retryCount; }
  int GetMaxRetries() const { return m_maxRetries; }
  bool ShouldRetry() const;
  std::chrono::steady_clock::time_point GetNextRetryTime() const {
    return m_nextRetryTime;
  }
  int GetRetryDelayMs() const; // Get current delay in milliseconds

  // Checksum support
  std::string GetExpectedChecksum() const;
  std::string GetCalculatedChecksum() const;
  int GetChecksumType() const {
    return m_checksumType;
  } // 0=None, 1=MD5, 2=SHA256
  bool IsChecksumVerified() const { return m_checksumVerified; }

  // Setters
  void SetFilename(const std::string &filename);
  void SetTotalSize(int64_t size) { m_totalSize.store(size); }
  void SetDownloadedSize(int64_t size) { m_downloadedSize = size; }
  void SetStatus(DownloadStatus status) { m_status = status; }
  void SetCategory(const std::string &category);
  void SetDescription(const std::string &desc);
  void SetSpeed(double speed) { m_speed = speed; }
  void SetErrorMessage(const std::string &msg);
  void SetSavePath(const std::string &path);
  void UpdateLastTryTime();

  // Retry support
  void SetMaxRetries(int maxRetries) { m_maxRetries = maxRetries; }
  void IncrementRetry(); // Increment retry count and calculate next retry time
  void ResetRetry();     // Reset retry count (on success or user restart)

  // Checksum support
  void SetExpectedChecksum(const std::string &hash, int type);
  void SetCalculatedChecksum(const std::string &hash);
  void SetChecksumVerified(bool verified) { m_checksumVerified = verified; }

  // Chunk management
  void InitializeChunks(int numConnections);
  std::vector<DownloadChunk> &GetChunks() { return m_chunks; }
  void UpdateChunkProgress(int chunkIndex, int64_t currentByte);

  // Progress calculation
  void RecalculateProgress();

private:
  int m_id;
  std::string m_url;
  std::string m_filename;
  std::string m_savePath;
  std::atomic<int64_t> m_totalSize;
  std::atomic<int64_t> m_downloadedSize;
  std::atomic<DownloadStatus> m_status;
  std::string m_category;
  std::string m_description;
  std::atomic<double> m_speed;
  std::string m_lastTryTime;
  std::string m_errorMessage;

  // Retry tracking for exponential backoff
  int m_retryCount = 0; // Current retry attempt (0 = first try)
  int m_maxRetries = 5; // Maximum retry attempts
  std::chrono::steady_clock::time_point m_nextRetryTime; // When to retry next

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
