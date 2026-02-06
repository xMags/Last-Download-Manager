#pragma once

#include "Download.h"
#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Video format info from yt-dlp
struct VideoFormat {
  std::string formatId;      // e.g., "137", "best"
  std::string resolution;    // e.g., "1920x1080", "1280x720"
  std::string ext;           // e.g., "mp4", "webm"
  std::string note;          // e.g., "1080p", "720p"
  int height;                // e.g., 1080, 720, 0 for audio-only
  int64_t filesize;          // Estimated size in bytes, -1 if unknown
  bool hasVideo;
  bool hasAudio;
};

// Manages yt-dlp integration for video site downloads
class YtDlpManager {
public:
  static YtDlpManager &GetInstance();

  // Disable copy
  YtDlpManager(const YtDlpManager &) = delete;
  YtDlpManager &operator=(const YtDlpManager &) = delete;

  // Check if yt-dlp.exe is available
  bool IsYtDlpAvailable() const;

  // Get yt-dlp.exe path
  std::string GetYtDlpPath() const;

  // Download yt-dlp.exe from GitHub (async, returns immediately)
  // Callback called with success/failure
  void DownloadYtDlp(std::function<void(bool success, const std::string &error)> callback);

  // Check if URL is a video site that needs yt-dlp
  bool IsVideoSiteUrl(const std::string &url) const;

  // Get available formats for a URL (blocking call)
  std::vector<VideoFormat> GetAvailableFormats(const std::string &url) const;

  // Get video title for a URL (blocking call)
  std::string GetVideoTitle(const std::string &url) const;

  // Start a video download using yt-dlp
  bool StartDownload(std::shared_ptr<Download> download);

  // Start download with specific format
  bool StartDownloadWithFormat(std::shared_ptr<Download> download, const std::string &formatId);

  // Pause a yt-dlp download (kills process, can resume)
  void PauseDownload(int downloadId);

  // Resume a paused yt-dlp download
  void ResumeDownload(std::shared_ptr<Download> download);

  // Cancel a yt-dlp download
  void CancelDownload(int downloadId);

  // Wait for a yt-dlp download task to finish
  // Returns true if download is no longer running, false on timeout
  bool WaitForDownloadFinish(int downloadId, int timeoutMs = 5000);

  // Set the output directory for downloads
  void SetOutputDirectory(const std::string &dir);

  // Get yt-dlp version (empty if not installed)
  std::string GetYtDlpVersion() const;

  // Update yt-dlp to latest version
  void UpdateYtDlp(std::function<void(bool success, const std::string &error)> callback);

  // Check if ffmpeg is available
  bool IsFfmpegAvailable() const;

  // Download ffmpeg (async)
  void DownloadFfmpeg(std::function<void(bool success, const std::string &error)> callback);

  // Check if Deno (JS runtime) is available
  bool IsDenoAvailable() const;

  // Download Deno (async) - required for YouTube
  void DownloadDeno(std::function<void(bool success, const std::string &error)> callback);

private:
  YtDlpManager();
  ~YtDlpManager();

  // Internal download function (runs in thread)
  void PerformDownload(std::shared_ptr<Download> download);

  // Parse yt-dlp progress output
  void ParseProgressLine(const std::string &line, std::shared_ptr<Download> download);

  // Kill yt-dlp process for a download
  void KillProcess(int downloadId);

  // Clean up completed download tasks
  void CleanupCompletedTasks();

  // Supported video sites (partial list - yt-dlp supports 1400+)
  static const std::unordered_set<std::string> VIDEO_SITE_PATTERNS;

  std::string m_ytdlpPath;
  std::string m_ffmpegPath;
  std::string m_denoPath;
  std::string m_outputDir;
  mutable std::mutex m_mutex;

  // Track running yt-dlp processes (downloadId -> process handle)
  std::unordered_map<int, void*> m_runningProcesses;
  std::mutex m_processMutex;

  // Download tasks using std::future for proper lifecycle management
  std::unordered_map<int, std::future<void>> m_downloadTasks;
  std::mutex m_tasksMutex;

  // Format selection per download (downloadId -> formatId)
  std::unordered_map<int, std::string> m_downloadFormats;
  std::mutex m_formatsMutex;

  // Utility threads (download yt-dlp, ffmpeg, deno)
  std::vector<std::future<void>> m_utilityTasks;
  std::mutex m_utilityTasksMutex;

  // Flag for shutdown
  std::atomic<bool> m_shuttingDown{false};
};
