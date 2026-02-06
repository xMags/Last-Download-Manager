#include "YtDlpManager.h"
#include "../utils/Settings.h"
#include <Windows.h>
#include <ShlObj.h>
#include <WinInet.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <thread>

#pragma comment(lib, "wininet.lib")

// Helper function to run a command silently and capture output (with timeout)
static std::string RunCommandSilent(const std::string &cmd, int timeoutMs = 30000) {
  std::string output;

  SECURITY_ATTRIBUTES sa;
  sa.nLength = sizeof(SECURITY_ATTRIBUTES);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = NULL;

  HANDLE hReadPipe = NULL, hWritePipe = NULL;
  if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
    return output;
  }

  SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOA si = {0};
  si.cb = sizeof(STARTUPINFOA);
  si.hStdOutput = hWritePipe;
  si.hStdError = hWritePipe;
  si.dwFlags |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;

  PROCESS_INFORMATION pi = {0};

  std::vector<char> cmdLine(cmd.begin(), cmd.end());
  cmdLine.push_back('\0');

  if (!CreateProcessA(NULL, cmdLine.data(), NULL, NULL, TRUE,
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
    CloseHandle(hReadPipe);
    CloseHandle(hWritePipe);
    return output;
  }

  // Close write end in parent - must do this before reading
  CloseHandle(hWritePipe);
  hWritePipe = NULL;

  // Wait for process with timeout
  DWORD waitResult = WaitForSingleObject(pi.hProcess, timeoutMs);

  if (waitResult == WAIT_TIMEOUT) {
    // Process timed out - terminate it
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, 1000); // Wait for termination
  }

  // Now read all available output (process has exited or been terminated)
  char buffer[4096];
  DWORD bytesRead;
  while (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
    buffer[bytesRead] = '\0';
    output += buffer;
  }

  // Clean up handles
  CloseHandle(hReadPipe);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);

  return output;
}

// Helper function to run a command silently without capturing output
static int RunCommandSilentNoOutput(const std::string &cmd, int timeoutMs = 120000) {
  STARTUPINFOA si = {0};
  si.cb = sizeof(STARTUPINFOA);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;

  PROCESS_INFORMATION pi = {0};

  std::vector<char> cmdLine(cmd.begin(), cmd.end());
  cmdLine.push_back('\0');

  if (!CreateProcessA(NULL, cmdLine.data(), NULL, NULL, FALSE,
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
    return -1;
  }

  DWORD waitResult = WaitForSingleObject(pi.hProcess, timeoutMs);

  DWORD exitCode = 0;
  if (waitResult == WAIT_TIMEOUT) {
    // Process timed out - terminate it
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, 1000);
    exitCode = static_cast<DWORD>(-2);  // Timeout error code
  } else {
    GetExitCodeProcess(pi.hProcess, &exitCode);
  }

  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);

  return static_cast<int>(exitCode);
}

// Video sites that yt-dlp can handle (subset - it supports 1400+)
// Note: pornhub removed due to causing app freezes
const std::unordered_set<std::string> YtDlpManager::VIDEO_SITE_PATTERNS = {
    "youtube.com", "youtu.be", "youtube-nocookie.com",
    "vimeo.com", "dailymotion.com",
    "twitter.com", "x.com", "t.co",
    "facebook.com", "fb.watch", "instagram.com",
    "tiktok.com", "vm.tiktok.com",
    "twitch.tv", "clips.twitch.tv",
    "reddit.com", "v.redd.it",
    "streamable.com", "gfycat.com", "imgur.com",
    "bilibili.com", "nicovideo.jp",
    "soundcloud.com", "bandcamp.com",
    "xvideos.com", "xhamster.com",
    "crunchyroll.com", "funimation.com",
    "ted.com", "vk.com", "ok.ru",
    "rumble.com", "bitchute.com", "odysee.com",
    "mixcloud.com", "audiomack.com",
    "hotstar.com", "zee5.com", "sonyliv.com",
    "mediafire.com", "zippyshare.com"
};

// GitHub release URL for yt-dlp
static const char* YTDLP_DOWNLOAD_URL =
    "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe";

YtDlpManager &YtDlpManager::GetInstance() {
  static YtDlpManager instance;
  return instance;
}

YtDlpManager::YtDlpManager() {
  // Set default path: AppData/LDM/tools/yt-dlp.exe
  char appDataPath[MAX_PATH];
  if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appDataPath))) {
    std::string appDataStr(appDataPath);
    m_ytdlpPath = appDataStr + "\\LDM\\tools\\yt-dlp.exe";
    m_ffmpegPath = appDataStr + "\\LDM\\tools\\ffmpeg.exe";
    m_denoPath = appDataStr + "\\LDM\\tools\\deno.exe";
    m_outputDir = appDataStr + "\\LDM\\downloads";

    // Create tools directory if needed
    std::string ldmDir = appDataStr + "\\LDM";
    std::string toolsDir = ldmDir + "\\tools";
    CreateDirectoryA(ldmDir.c_str(), NULL);
    CreateDirectoryA(toolsDir.c_str(), NULL);
  } else {
    m_ytdlpPath = "yt-dlp.exe";
    m_ffmpegPath = "ffmpeg.exe";
    m_denoPath = "deno.exe";
    m_outputDir = ".";
  }
}

YtDlpManager::~YtDlpManager() {
  // Set shutdown flag to stop any loops
  m_shuttingDown.store(true);

  // Kill all running processes first
  {
    std::lock_guard<std::mutex> lock(m_processMutex);
    for (auto &pair : m_runningProcesses) {
      if (pair.second) {
        TerminateProcess(pair.second, 1);
        CloseHandle(pair.second);
      }
    }
    m_runningProcesses.clear();
  }

  // Wait for all download tasks to complete (with timeout)
  {
    std::lock_guard<std::mutex> lock(m_tasksMutex);
    for (auto &pair : m_downloadTasks) {
      if (pair.second.valid()) {
        // Wait with timeout to avoid hanging
        auto status = pair.second.wait_for(std::chrono::seconds(2));
        if (status == std::future_status::timeout) {
          // Task didn't complete in time, it will be abandoned
          std::cerr << "[YtDlpManager] Warning: Task " << pair.first << " did not complete in time" << std::endl;
        }
      }
    }
    m_downloadTasks.clear();
  }

  // Wait for utility tasks (yt-dlp/ffmpeg/deno downloads) to complete
  {
    std::lock_guard<std::mutex> lock(m_utilityTasksMutex);
    for (auto &task : m_utilityTasks) {
      if (task.valid()) {
        auto status = task.wait_for(std::chrono::seconds(2));
        if (status == std::future_status::timeout) {
          std::cerr << "[YtDlpManager] Warning: Utility task did not complete in time" << std::endl;
        }
      }
    }
    m_utilityTasks.clear();
  }
}

bool YtDlpManager::IsYtDlpAvailable() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  DWORD attrs = GetFileAttributesA(m_ytdlpPath.c_str());
  return (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY));
}

std::string YtDlpManager::GetYtDlpPath() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_ytdlpPath;
}

void YtDlpManager::SetOutputDirectory(const std::string &dir) {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_outputDir = dir;
}

bool YtDlpManager::IsVideoSiteUrl(const std::string &url) const {
  // Convert to lowercase for matching
  std::string lowerUrl = url;
  std::transform(lowerUrl.begin(), lowerUrl.end(), lowerUrl.begin(), ::tolower);

  for (const auto &pattern : VIDEO_SITE_PATTERNS) {
    if (lowerUrl.find(pattern) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::vector<VideoFormat> YtDlpManager::GetAvailableFormats(const std::string &url) const {
  std::vector<VideoFormat> formats;

  std::string ytdlpPath;
  std::string denoPath;
  bool hasDeno = false;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    // Check if yt-dlp exists directly (don't call IsYtDlpAvailable to avoid deadlock)
    DWORD attrs = GetFileAttributesA(m_ytdlpPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
      return formats;
    }
    ytdlpPath = m_ytdlpPath;
    denoPath = m_denoPath;
    attrs = GetFileAttributesA(denoPath.c_str());
    hasDeno = (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY));
  }

  // Build command with Deno option only for YouTube
  bool isYouTube = (url.find("youtube.com") != std::string::npos ||
                    url.find("youtu.be") != std::string::npos ||
                    url.find("youtube-nocookie.com") != std::string::npos);
  std::string denoOption = (hasDeno && isYouTube) ? "--js-runtimes deno:\"" + denoPath + "\" " : "";

  // Run yt-dlp -F to list formats (silently, with timeout)
  std::string cmd = "\"" + ytdlpPath + "\" " + denoOption + "-F --no-playlist --socket-timeout 15 \"" + url + "\"";
  std::string output = RunCommandSilent(cmd, 20000); // 20 second timeout

  if (output.empty()) {
    return formats;
  }

  // Parse output lines
  // Format: ID  EXT   RESOLUTION FPS CH │   FILESIZE   TBR PROTO │ VCODEC          VBR ACODEC      ABR ASR MORE INFO
  std::istringstream stream(output);
  std::string line;
  bool headerPassed = false;

  while (std::getline(stream, line)) {
    // Skip header lines (before the separator line with dashes)
    if (line.find("---") != std::string::npos) {
      headerPassed = true;
      continue;
    }
    if (!headerPassed || line.empty()) continue;

    // Parse format line
    std::istringstream lineStream(line);
    std::string formatId, ext, resolution;
    lineStream >> formatId >> ext >> resolution;

    if (formatId.empty() || formatId[0] == '[') continue;

    VideoFormat fmt;
    fmt.formatId = formatId;
    fmt.ext = ext;
    fmt.resolution = resolution;
    fmt.filesize = -1;
    fmt.hasVideo = true;
    fmt.hasAudio = true;

    // Determine height from resolution
    if (resolution.find("x") != std::string::npos) {
      size_t xPos = resolution.find("x");
      try {
        fmt.height = std::stoi(resolution.substr(xPos + 1));
      } catch (...) {
        fmt.height = 0;
      }
    } else if (resolution.find("audio") != std::string::npos) {
      fmt.height = 0;
      fmt.hasVideo = false;
    } else {
      // Try to parse as height directly (e.g., "1080p")
      try {
        fmt.height = std::stoi(resolution);
      } catch (...) {
        fmt.height = 0;
      }
    }

    // Create readable note
    if (fmt.height > 0) {
      fmt.note = std::to_string(fmt.height) + "p " + ext;
    } else if (!fmt.hasVideo) {
      fmt.note = "Audio only (" + ext + ")";
    } else {
      fmt.note = ext;
    }

    // Check if it's video-only or audio-only from the line
    if (line.find("video only") != std::string::npos) {
      fmt.hasAudio = false;
      fmt.note += " (video only)";
    }
    if (line.find("audio only") != std::string::npos) {
      fmt.hasVideo = false;
      fmt.hasAudio = true;
    }

    formats.push_back(fmt);
  }

  // Check if ffmpeg is available for format presets
  bool hasFfmpeg = false;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    DWORD attrs = GetFileAttributesA(m_ffmpegPath.c_str());
    hasFfmpeg = (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY));
  }

  // Add convenient preset options at the beginning
  // Use different formats based on ffmpeg availability
  if (hasFfmpeg) {
    // With ffmpeg: can merge separate video+audio streams for best quality
    VideoFormat best;
    best.formatId = "bestvideo[ext=mp4]+bestaudio[ext=m4a]/bestvideo+bestaudio/best";
    best.resolution = "Best";
    best.ext = "mp4";
    best.note = "Best Quality (auto)";
    best.height = 9999;
    best.filesize = -1;
    best.hasVideo = true;
    best.hasAudio = true;
    formats.insert(formats.begin(), best);

    VideoFormat hd1080;
    hd1080.formatId = "bestvideo[height<=1080][ext=mp4]+bestaudio[ext=m4a]/bestvideo[height<=1080]+bestaudio/best[height<=1080]";
    hd1080.resolution = "1920x1080";
    hd1080.ext = "mp4";
    hd1080.note = "1080p HD (recommended)";
    hd1080.height = 1080;
    hd1080.filesize = -1;
    hd1080.hasVideo = true;
    hd1080.hasAudio = true;
    formats.insert(formats.begin() + 1, hd1080);

    VideoFormat hd720;
    hd720.formatId = "bestvideo[height<=720][ext=mp4]+bestaudio[ext=m4a]/bestvideo[height<=720]+bestaudio/best[height<=720]";
    hd720.resolution = "1280x720";
    hd720.ext = "mp4";
    hd720.note = "720p HD";
    hd720.height = 720;
    hd720.filesize = -1;
    hd720.hasVideo = true;
    hd720.hasAudio = true;
    formats.insert(formats.begin() + 2, hd720);

    VideoFormat sd480;
    sd480.formatId = "bestvideo[height<=480][ext=mp4]+bestaudio[ext=m4a]/bestvideo[height<=480]+bestaudio/best[height<=480]";
    sd480.resolution = "854x480";
    sd480.ext = "mp4";
    sd480.note = "480p SD";
    sd480.height = 480;
    sd480.filesize = -1;
    sd480.hasVideo = true;
    sd480.hasAudio = true;
    formats.insert(formats.begin() + 3, sd480);
  } else {
    // Without ffmpeg: must use pre-merged formats (mp4 with both video+audio)
    VideoFormat best;
    best.formatId = "best[ext=mp4]/best";
    best.resolution = "Best";
    best.ext = "mp4";
    best.note = "Best Quality (mp4)";
    best.height = 9999;
    best.filesize = -1;
    best.hasVideo = true;
    best.hasAudio = true;
    formats.insert(formats.begin(), best);

    VideoFormat hd720;
    hd720.formatId = "best[height<=720][ext=mp4]/best[height<=720]";
    hd720.resolution = "1280x720";
    hd720.ext = "mp4";
    hd720.note = "720p (max without ffmpeg)";
    hd720.height = 720;
    hd720.filesize = -1;
    hd720.hasVideo = true;
    hd720.hasAudio = true;
    formats.insert(formats.begin() + 1, hd720);

    VideoFormat sd480;
    sd480.formatId = "best[height<=480][ext=mp4]/best[height<=480]";
    sd480.resolution = "854x480";
    sd480.ext = "mp4";
    sd480.note = "480p SD";
    sd480.height = 480;
    sd480.filesize = -1;
    sd480.hasVideo = true;
    sd480.hasAudio = true;
    formats.insert(formats.begin() + 2, sd480);
  }

  return formats;
}

std::string YtDlpManager::GetVideoTitle(const std::string &url) const {
  std::string ytdlpPath;
  std::string denoPath;
  bool hasDeno = false;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    // Check if yt-dlp exists directly (don't call IsYtDlpAvailable to avoid deadlock)
    DWORD attrs = GetFileAttributesA(m_ytdlpPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
      return "";
    }
    ytdlpPath = m_ytdlpPath;
    denoPath = m_denoPath;
    attrs = GetFileAttributesA(denoPath.c_str());
    hasDeno = (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY));
  }

  // Build command with Deno option only for YouTube
  bool isYouTube = (url.find("youtube.com") != std::string::npos ||
                    url.find("youtu.be") != std::string::npos ||
                    url.find("youtube-nocookie.com") != std::string::npos);
  std::string denoOption = (hasDeno && isYouTube) ? "--js-runtimes deno:\"" + denoPath + "\" " : "";

  std::string cmd = "\"" + ytdlpPath + "\" " + denoOption + "--get-title --no-playlist --socket-timeout 15 \"" + url + "\"";
  std::string result = RunCommandSilent(cmd, 20000); // 20 second timeout

  // Trim whitespace
  size_t end = result.find_last_not_of(" \n\r\t");
  if (end != std::string::npos) {
    result = result.substr(0, end + 1);
  }
  return result;
}

std::string YtDlpManager::GetYtDlpVersion() const {
  // Get path while holding mutex
  std::string ytdlpPath;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    DWORD attrs = GetFileAttributesA(m_ytdlpPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
      return "";
    }
    ytdlpPath = m_ytdlpPath;
  }

  std::string cmd = "\"" + ytdlpPath + "\" --version";
  std::string result = RunCommandSilent(cmd, 5000); // 5 second timeout

  // Trim whitespace
  size_t end = result.find_last_not_of(" \n\r\t");
  if (end != std::string::npos) {
    result = result.substr(0, end + 1);
  }
  return result;
}

void YtDlpManager::DownloadYtDlp(std::function<void(bool, const std::string &)> callback) {
  // Get path while holding mutex
  std::string ytdlpPath;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    ytdlpPath = m_ytdlpPath;
  }

  // Clean up completed utility tasks first
  {
    std::lock_guard<std::mutex> lock(m_utilityTasksMutex);
    m_utilityTasks.erase(
        std::remove_if(m_utilityTasks.begin(), m_utilityTasks.end(),
                       [](std::future<void> &f) {
                         return !f.valid() ||
                                f.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
                       }),
        m_utilityTasks.end());
  }

  auto future = std::async(std::launch::async, [ytdlpPath, callback]() {
    try {
      std::cout << "[YtDlpManager] Downloading yt-dlp from GitHub..." << std::endl;

      HINTERNET hInternet = InternetOpenA(
          "LDM/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);

      if (!hInternet) {
        callback(false, "Failed to initialize internet connection");
        return;
      }

      HINTERNET hUrl = InternetOpenUrlA(
          hInternet, YTDLP_DOWNLOAD_URL, NULL, 0,
          INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);

      if (!hUrl) {
        InternetCloseHandle(hInternet);
        callback(false, "Failed to connect to GitHub");
        return;
      }

      // Create temp file first, then move
      std::string tempPath = ytdlpPath + ".tmp";
      std::ofstream outFile(tempPath, std::ios::binary);

      if (!outFile.is_open()) {
        InternetCloseHandle(hUrl);
        InternetCloseHandle(hInternet);
        callback(false, "Failed to create output file");
        return;
      }

      char buffer[8192];
      DWORD bytesRead;
      DWORD totalBytes = 0;

      while (InternetReadFile(hUrl, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
        outFile.write(buffer, bytesRead);
        totalBytes += bytesRead;
      }

      outFile.close();
      InternetCloseHandle(hUrl);
      InternetCloseHandle(hInternet);

      if (totalBytes < 1000000) {  // yt-dlp.exe should be ~10MB+
        DeleteFileA(tempPath.c_str());
        callback(false, "Download incomplete or failed");
        return;
      }

      // Move temp file to final location
      DeleteFileA(ytdlpPath.c_str());  // Remove old version if exists
      if (!MoveFileA(tempPath.c_str(), ytdlpPath.c_str())) {
        DeleteFileA(tempPath.c_str());
        callback(false, "Failed to save yt-dlp.exe");
        return;
      }

      std::cout << "[YtDlpManager] yt-dlp downloaded successfully ("
                << (totalBytes / 1024 / 1024) << " MB)" << std::endl;
      callback(true, "");

    } catch (const std::exception &e) {
      callback(false, e.what());
    }
  });

  // Track the task for cleanup on shutdown
  {
    std::lock_guard<std::mutex> lock(m_utilityTasksMutex);
    m_utilityTasks.push_back(std::move(future));
  }
}

void YtDlpManager::UpdateYtDlp(std::function<void(bool, const std::string &)> callback) {
  // Same as download - it will overwrite
  DownloadYtDlp(callback);
}

bool YtDlpManager::IsFfmpegAvailable() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  DWORD attrs = GetFileAttributesA(m_ffmpegPath.c_str());
  return (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY));
}

void YtDlpManager::DownloadFfmpeg(std::function<void(bool, const std::string &)> callback) {
  // Get path while holding mutex
  std::string ffmpegPath;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    ffmpegPath = m_ffmpegPath;
  }

  std::string toolsDir = ffmpegPath.substr(0, ffmpegPath.find_last_of("\\/"));

  // Clean up completed utility tasks first
  {
    std::lock_guard<std::mutex> lock(m_utilityTasksMutex);
    m_utilityTasks.erase(
        std::remove_if(m_utilityTasks.begin(), m_utilityTasks.end(),
                       [](std::future<void> &f) {
                         return !f.valid() ||
                                f.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
                       }),
        m_utilityTasks.end());
  }

  auto future = std::async(std::launch::async, [ffmpegPath, toolsDir, callback]() {
    try {
      std::cout << "[YtDlpManager] Downloading ffmpeg..." << std::endl;

      // Download from yt-dlp's recommended ffmpeg builds
      const char* FFMPEG_URL =
          "https://github.com/yt-dlp/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip";

      HINTERNET hInternet = InternetOpenA(
          "LDM/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);

      if (!hInternet) {
        callback(false, "Failed to initialize internet connection");
        return;
      }

      HINTERNET hUrl = InternetOpenUrlA(
          hInternet, FFMPEG_URL, NULL, 0,
          INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);

      if (!hUrl) {
        InternetCloseHandle(hInternet);
        callback(false, "Failed to connect to GitHub");
        return;
      }

      // Download to temp zip file
      std::string zipPath = toolsDir + "\\ffmpeg.zip";
      std::ofstream outFile(zipPath, std::ios::binary);

      if (!outFile.is_open()) {
        InternetCloseHandle(hUrl);
        InternetCloseHandle(hInternet);
        callback(false, "Failed to create output file");
        return;
      }

      char buffer[8192];
      DWORD bytesRead;
      DWORD totalBytes = 0;

      while (InternetReadFile(hUrl, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
        outFile.write(buffer, bytesRead);
        totalBytes += bytesRead;
      }

      outFile.close();
      InternetCloseHandle(hUrl);
      InternetCloseHandle(hInternet);

      std::cout << "[YtDlpManager] Downloaded " << (totalBytes / 1024 / 1024) << " MB" << std::endl;

      if (totalBytes < 1000000) {  // ffmpeg should be ~80MB+
        DeleteFileA(zipPath.c_str());
        callback(false, "Download incomplete");
        return;
      }

      // Extract ffmpeg.exe from zip using PowerShell
      // First extract to temp folder, then find and copy ffmpeg.exe
      std::string tempDir = toolsDir + "\\ffmpeg_temp";
      std::string extractCmd =
          "powershell -ExecutionPolicy Bypass -Command \""
          "Expand-Archive -Path '" + zipPath + "' -DestinationPath '" + tempDir + "' -Force; "
          "$ffmpegExe = Get-ChildItem -Path '" + tempDir + "' -Recurse -Filter 'ffmpeg.exe' | Select-Object -First 1; "
          "if ($ffmpegExe) { Copy-Item $ffmpegExe.FullName '" + ffmpegPath + "' -Force; Write-Host 'OK' } else { Write-Host 'NOT_FOUND' }; "
          "Remove-Item '" + tempDir + "' -Recurse -Force -ErrorAction SilentlyContinue"
          "\"";

      std::cout << "[YtDlpManager] Extracting ffmpeg..." << std::endl;
      RunCommandSilentNoOutput(extractCmd);
      DeleteFileA(zipPath.c_str());

      // Verify ffmpeg was extracted
      DWORD attrs = GetFileAttributesA(ffmpegPath.c_str());
      if (attrs == INVALID_FILE_ATTRIBUTES) {
        callback(false, "ffmpeg extraction failed - file not found after extraction");
        return;
      }

      std::cout << "[YtDlpManager] ffmpeg downloaded successfully" << std::endl;
      callback(true, "");

    } catch (const std::exception &e) {
      callback(false, e.what());
    }
  });

  // Track the task for cleanup on shutdown
  {
    std::lock_guard<std::mutex> lock(m_utilityTasksMutex);
    m_utilityTasks.push_back(std::move(future));
  }
}

bool YtDlpManager::IsDenoAvailable() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  DWORD attrs = GetFileAttributesA(m_denoPath.c_str());
  return (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY));
}

void YtDlpManager::DownloadDeno(std::function<void(bool, const std::string &)> callback) {
  // Get path while holding mutex
  std::string denoPath;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    denoPath = m_denoPath;
  }

  std::string toolsDir = denoPath.substr(0, denoPath.find_last_of("\\/"));

  // Clean up completed utility tasks first
  {
    std::lock_guard<std::mutex> lock(m_utilityTasksMutex);
    m_utilityTasks.erase(
        std::remove_if(m_utilityTasks.begin(), m_utilityTasks.end(),
                       [](std::future<void> &f) {
                         return !f.valid() ||
                                f.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
                       }),
        m_utilityTasks.end());
  }

  auto future = std::async(std::launch::async, [denoPath, toolsDir, callback]() {
    try {
      std::cout << "[YtDlpManager] Downloading Deno (JavaScript runtime)..." << std::endl;

      // Download Deno from GitHub releases
      const char* DENO_URL =
          "https://github.com/denoland/deno/releases/latest/download/deno-x86_64-pc-windows-msvc.zip";

      HINTERNET hInternet = InternetOpenA(
          "LDM/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);

      if (!hInternet) {
        callback(false, "Failed to initialize internet connection");
        return;
      }

      HINTERNET hUrl = InternetOpenUrlA(
          hInternet, DENO_URL, NULL, 0,
          INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);

      if (!hUrl) {
        InternetCloseHandle(hInternet);
        callback(false, "Failed to connect to GitHub");
        return;
      }

      // Download to temp zip file
      std::string zipPath = toolsDir + "\\deno.zip";
      std::ofstream outFile(zipPath, std::ios::binary);

      if (!outFile.is_open()) {
        InternetCloseHandle(hUrl);
        InternetCloseHandle(hInternet);
        callback(false, "Failed to create output file");
        return;
      }

      char buffer[8192];
      DWORD bytesRead;
      DWORD totalBytes = 0;

      while (InternetReadFile(hUrl, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
        outFile.write(buffer, bytesRead);
        totalBytes += bytesRead;
      }

      outFile.close();
      InternetCloseHandle(hUrl);
      InternetCloseHandle(hInternet);

      std::cout << "[YtDlpManager] Downloaded " << (totalBytes / 1024 / 1024) << " MB" << std::endl;

      if (totalBytes < 1000000) {  // Deno should be ~30MB+
        DeleteFileA(zipPath.c_str());
        callback(false, "Download incomplete");
        return;
      }

      // Extract deno.exe from zip using PowerShell
      std::string extractCmd =
          "powershell -ExecutionPolicy Bypass -Command \""
          "Expand-Archive -Path '" + zipPath + "' -DestinationPath '" + toolsDir + "' -Force"
          "\"";

      std::cout << "[YtDlpManager] Extracting Deno..." << std::endl;
      RunCommandSilentNoOutput(extractCmd);
      DeleteFileA(zipPath.c_str());

      // Verify deno was extracted
      DWORD attrs = GetFileAttributesA(denoPath.c_str());
      if (attrs == INVALID_FILE_ATTRIBUTES) {
        callback(false, "Deno extraction failed - file not found");
        return;
      }

      std::cout << "[YtDlpManager] Deno downloaded successfully" << std::endl;
      callback(true, "");

    } catch (const std::exception &e) {
      callback(false, e.what());
    }
  });

  // Track the task for cleanup on shutdown
  {
    std::lock_guard<std::mutex> lock(m_utilityTasksMutex);
    m_utilityTasks.push_back(std::move(future));
  }
}

void YtDlpManager::CleanupCompletedTasks() {
  std::lock_guard<std::mutex> lock(m_tasksMutex);
  for (auto it = m_downloadTasks.begin(); it != m_downloadTasks.end();) {
    if (it->second.valid()) {
      // Check if ready without blocking
      auto status = it->second.wait_for(std::chrono::milliseconds(0));
      if (status == std::future_status::ready) {
        // Get the result to clear any stored exception
        try {
          it->second.get();
        } catch (...) {
          // Ignore exceptions from completed tasks
        }
        it = m_downloadTasks.erase(it);
        continue;
      }
    }
    ++it;
  }
}

bool YtDlpManager::StartDownload(std::shared_ptr<Download> download) {
  // Default: use best quality with ffmpeg if available
  return StartDownloadWithFormat(download, "");
}

bool YtDlpManager::StartDownloadWithFormat(std::shared_ptr<Download> download, const std::string &formatId) {
  if (!download) {
    return false;
  }

  if (!IsYtDlpAvailable()) {
    download->SetStatus(DownloadStatus::Error);
    download->SetErrorMessage("yt-dlp not installed");
    return false;
  }

  int downloadId = download->GetId();

  // Store format selection
  if (!formatId.empty()) {
    std::lock_guard<std::mutex> lock(m_formatsMutex);
    m_downloadFormats[downloadId] = formatId;
  }

  // Mark the download object itself as yt-dlp download
  download->SetYtDlpDownload(true);
  download->SetStatus(DownloadStatus::Downloading);

  // Clean up any completed tasks first
  CleanupCompletedTasks();

  // Start download using std::async for proper lifecycle management
  auto future = std::async(std::launch::async, [this, download]() {
    PerformDownload(download);
  });

  {
    std::lock_guard<std::mutex> lock(m_tasksMutex);
    m_downloadTasks[downloadId] = std::move(future);
  }

  return true;
}

void YtDlpManager::PerformDownload(std::shared_ptr<Download> download) {
  if (!download || m_shuttingDown.load()) return;

  int downloadId = download->GetId();
  std::string url = download->GetUrl();
  std::string outputDir = download->GetSavePath();

  // Get selected format for this download
  std::string selectedFormat;
  {
    std::lock_guard<std::mutex> lock(m_formatsMutex);
    auto it = m_downloadFormats.find(downloadId);
    if (it != m_downloadFormats.end()) {
      selectedFormat = it->second;
      m_downloadFormats.erase(it);  // Clean up after reading
    }
  }

  // Get yt-dlp path, ffmpeg path, deno path, and output dir safely
  std::string ytdlpPath;
  std::string ffmpegPath;
  std::string denoPath;
  bool hasFfmpeg = false;
  bool hasDeno = false;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    ytdlpPath = m_ytdlpPath;
    ffmpegPath = m_ffmpegPath;
    denoPath = m_denoPath;
    if (outputDir.empty()) {
      outputDir = m_outputDir;
    }
    // Check if ffmpeg exists
    DWORD attrs = GetFileAttributesA(ffmpegPath.c_str());
    hasFfmpeg = (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY));
    // Check if deno exists
    attrs = GetFileAttributesA(denoPath.c_str());
    hasDeno = (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY));
  }

  // Ensure output directory exists and is valid
  if (outputDir.empty()) {
    // Fallback to user's Videos folder
    char videosPath[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_MYVIDEO, NULL, 0, videosPath))) {
      outputDir = std::string(videosPath) + "\\LDM";
    } else {
      outputDir = ".";
    }
  }

  // Create output directory
  CreateDirectoryA(outputDir.c_str(), NULL);

  std::cout << "[YtDlpManager] Output directory: " << outputDir << std::endl;
  std::cout << "[YtDlpManager] ffmpeg available: " << (hasFfmpeg ? "yes" : "no") << std::endl;
  std::cout << "[YtDlpManager] Deno available: " << (hasDeno ? "yes" : "no") << std::endl;
  std::cout << "[YtDlpManager] Selected format: " << (selectedFormat.empty() ? "auto" : selectedFormat) << std::endl;

  // Build yt-dlp command
  // --newline: progress on new lines for parsing
  // --no-mtime: don't set file mtime to upload date
  // -o: output template
  // --progress: show progress
  // -c: continue partial downloads (resume support)
  // --no-playlist: only download single video, not playlist
  // --restrict-filenames: avoid special characters in filename
  std::string outputTemplate = outputDir + "\\%(title)s.%(ext)s";

  std::string formatOption;
  std::string mergeOption;
  std::string ffmpegOption;
  std::string denoOption;

  // Configure Deno JS runtime only for YouTube (it's the main site that requires it)
  bool isYouTube = (url.find("youtube.com") != std::string::npos ||
                    url.find("youtu.be") != std::string::npos ||
                    url.find("youtube-nocookie.com") != std::string::npos);
  if (hasDeno && isYouTube) {
    denoOption = "--js-runtimes deno:\"" + denoPath + "\" ";
  }

  // Use selected format if specified, otherwise use defaults
  if (!selectedFormat.empty()) {
    formatOption = "-f \"" + selectedFormat + "\" ";
    if (hasFfmpeg) {
      mergeOption = "--merge-output-format mp4 ";
      std::string ffmpegDir = ffmpegPath.substr(0, ffmpegPath.find_last_of("\\/"));
      ffmpegOption = "--ffmpeg-location \"" + ffmpegDir + "\" ";
    }
  } else if (hasFfmpeg) {
    // With ffmpeg: download best video + audio and merge
    formatOption = "-f \"bestvideo[height<=1080][ext=mp4]+bestaudio[ext=m4a]/bestvideo[height<=1080]+bestaudio/best[height<=1080]/best\" ";
    mergeOption = "--merge-output-format mp4 ";
    // Tell yt-dlp where to find ffmpeg
    std::string ffmpegDir = ffmpegPath.substr(0, ffmpegPath.find_last_of("\\/"));
    ffmpegOption = "--ffmpeg-location \"" + ffmpegDir + "\" ";
  } else {
    // Without ffmpeg: get best pre-merged format (mp4 preferred)
    formatOption = "-f \"best[ext=mp4][height<=1080]/best[height<=1080]/best\" ";
  }

  std::string cmd = "\"" + ytdlpPath + "\" "
                    "--newline "
                    "--no-mtime "
                    "--progress "
                    "-c "
                    "--no-playlist "
                    "--restrict-filenames "
                    "--windows-filenames "
                    "--no-part "
                    "--no-warnings "
                    "--socket-timeout 30 "
                    "--retries 3 "
                    + denoOption
                    + ffmpegOption
                    + formatOption
                    + mergeOption
                    + "-o \"" + outputTemplate + "\" "
                    "\"" + url + "\"";

  std::cout << "[YtDlpManager] Running: " << cmd << std::endl;

  // Create process with pipes to read output
  SECURITY_ATTRIBUTES sa;
  sa.nLength = sizeof(SECURITY_ATTRIBUTES);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = NULL;

  HANDLE hReadPipe, hWritePipe;
  if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
    download->SetStatus(DownloadStatus::Error);
    download->SetErrorMessage("Failed to create pipe");
    return;
  }

  SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOA si = {0};
  si.cb = sizeof(STARTUPINFOA);
  si.hStdOutput = hWritePipe;
  si.hStdError = hWritePipe;
  si.dwFlags |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;

  PROCESS_INFORMATION pi = {0};

  // Create the process
  std::vector<char> cmdLine(cmd.begin(), cmd.end());
  cmdLine.push_back('\0');

  if (!CreateProcessA(NULL, cmdLine.data(), NULL, NULL, TRUE,
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
    CloseHandle(hReadPipe);
    CloseHandle(hWritePipe);
    download->SetStatus(DownloadStatus::Error);
    download->SetErrorMessage("Failed to start yt-dlp");
    return;
  }

  CloseHandle(hWritePipe);  // Close write end in parent

  // Store process handle for pause/cancel
  {
    std::lock_guard<std::mutex> lock(m_processMutex);
    m_runningProcesses[downloadId] = pi.hProcess;
  }

  // Read output and parse progress with timeout
  char buffer[4096];
  DWORD bytesRead;
  DWORD bytesAvailable;
  std::string lineBuffer;
  DWORD lastActivityTime = GetTickCount();
  const DWORD READ_TIMEOUT_MS = 120000; // 2 minute timeout for no output

  while (!m_shuttingDown.load()) {
    // Check for timeout (no output for too long)
    if (GetTickCount() - lastActivityTime > READ_TIMEOUT_MS) {
      std::cerr << "[YtDlpManager] Download timed out - no output for 2 minutes" << std::endl;
      TerminateProcess(pi.hProcess, 1);
      break;
    }

    // Check if data available (non-blocking)
    if (!PeekNamedPipe(hReadPipe, NULL, 0, NULL, &bytesAvailable, NULL)) {
      break; // Pipe closed or error
    }

    if (bytesAvailable > 0) {
      if (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
        lineBuffer += buffer;
        lastActivityTime = GetTickCount(); // Reset timeout on activity

        // Process complete lines
        size_t newlinePos;
        while ((newlinePos = lineBuffer.find('\n')) != std::string::npos) {
          std::string line = lineBuffer.substr(0, newlinePos);
          lineBuffer = lineBuffer.substr(newlinePos + 1);

          // Remove carriage return if present
          if (!line.empty() && line.back() == '\r') {
            line.pop_back();
          }

          ParseProgressLine(line, download);
        }
      }
    } else {
      // No data available, check if process is still running
      DWORD exitCode;
      if (GetExitCodeProcess(pi.hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
        // Process exited, read any remaining data
        while (PeekNamedPipe(hReadPipe, NULL, 0, NULL, &bytesAvailable, NULL) && bytesAvailable > 0) {
          if (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
            buffer[bytesRead] = '\0';
            lineBuffer += buffer;
          } else {
            break;
          }
        }
        break;
      }
      Sleep(100); // Small delay before next check
    }

    // Check if download was cancelled
    DownloadStatus status = download->GetStatus();
    if (status == DownloadStatus::Cancelled ||
        status == DownloadStatus::Paused) {
      break;
    }
  }

  CloseHandle(hReadPipe);

  // Wait for process to exit
  DWORD exitCode = 0;
  WaitForSingleObject(pi.hProcess, 5000);
  GetExitCodeProcess(pi.hProcess, &exitCode);

  // Clean up process handle - check if it's still in the map (not killed by user)
  bool wasKilled = false;
  {
    std::lock_guard<std::mutex> lock(m_processMutex);
    auto it = m_runningProcesses.find(downloadId);
    if (it != m_runningProcesses.end()) {
      // Still in map, we can close it
      CloseHandle(pi.hProcess);
      m_runningProcesses.erase(it);
    } else {
      // Already removed by KillProcess, don't close again
      wasKilled = true;
    }
  }

  CloseHandle(pi.hThread);

  // Set final status (only if not shutting down)
  if (!m_shuttingDown.load()) {
    DownloadStatus currentStatus = download->GetStatus();
    if (currentStatus != DownloadStatus::Cancelled &&
        currentStatus != DownloadStatus::Paused) {
      if (exitCode == 0) {
        download->SetStatus(DownloadStatus::Completed);
        download->SetProgress(100.0);
        std::cout << "[YtDlpManager] Download completed: " << download->GetFilename() << std::endl;
      } else {
        download->SetStatus(DownloadStatus::Error);
        // Keep existing error message if one was set during parsing, otherwise use generic
        std::string existingError = download->GetErrorMessage();
        if (existingError.empty() || existingError.find("yt-dlp exited") != std::string::npos) {
          download->SetErrorMessage("yt-dlp exited with code " + std::to_string(exitCode) +
                                    ". Try running yt-dlp manually to see the error.");
        }
        std::cerr << "[YtDlpManager] Download failed with exit code: " << exitCode << std::endl;
        std::cerr << "[YtDlpManager] Error message: " << download->GetErrorMessage() << std::endl;
        std::cerr << "[YtDlpManager] URL: " << download->GetUrl() << std::endl;
      }
    }
  }
}

void YtDlpManager::ParseProgressLine(const std::string &line, std::shared_ptr<Download> download) {
  if (!download || line.empty()) return;

  // Log ALL output from yt-dlp for debugging
  std::cout << "[yt-dlp] " << line << std::endl;

  // yt-dlp progress format examples:
  // [download]   5.2% of   50.23MiB at    2.50MiB/s ETA 00:19
  // [download]  45.3% of  150.23MiB at   10.50MiB/s ETA 00:09
  // [download] Destination: C:\path\to\Video Title.mp4
  // [download] 100% of 50.23MiB in 00:05
  // [Merger] Merging formats into "C:\path\to\Video Title.mp4"

  // Handle merger output (final filename after merging)
  if (line.find("[Merger]") != std::string::npos && line.find("Merging formats into") != std::string::npos) {
    size_t quoteStart = line.find('"');
    size_t quoteEnd = line.rfind('"');
    if (quoteStart != std::string::npos && quoteEnd != std::string::npos && quoteEnd > quoteStart) {
      std::string fullPath = line.substr(quoteStart + 1, quoteEnd - quoteStart - 1);

      // Extract directory and filename
      size_t lastSlash = fullPath.find_last_of("\\/");
      if (lastSlash != std::string::npos) {
        std::string dir = fullPath.substr(0, lastSlash);
        std::string filename = fullPath.substr(lastSlash + 1);
        download->SetSavePath(dir);
        download->SetFilename(filename);
      } else {
        download->SetFilename(fullPath);
      }
    }
    return;
  }

  if (line.find("[download]") != std::string::npos) {
    // Extract filename from Destination line
    if (line.find("Destination:") != std::string::npos) {
      size_t pos = line.find("Destination:");
      if (pos != std::string::npos) {
        std::string fullPath = line.substr(pos + 12);
        // Trim whitespace
        size_t start = fullPath.find_first_not_of(" \t");
        size_t end = fullPath.find_last_not_of(" \t");
        if (start != std::string::npos && end != std::string::npos) {
          fullPath = fullPath.substr(start, end - start + 1);
        }

        // Extract directory and filename
        size_t lastSlash = fullPath.find_last_of("\\/");
        if (lastSlash != std::string::npos) {
          std::string dir = fullPath.substr(0, lastSlash);
          std::string filename = fullPath.substr(lastSlash + 1);
          download->SetSavePath(dir);
          download->SetFilename(filename);
        } else {
          download->SetFilename(fullPath);
        }
      }
      return;
    }

    // Extract progress percentage
    std::regex progressRegex(R"(\s*(\d+\.?\d*)%\s+of\s+~?(\d+\.?\d*)(Ki|Mi|Gi)?B)");
    std::smatch match;
    if (std::regex_search(line, match, progressRegex)) {
      double progress = std::stod(match[1].str());
      download->SetProgress(progress);

      // Extract total size
      double size = std::stod(match[2].str());
      std::string unit = match[3].str();
      if (unit == "Ki") size *= 1024;
      else if (unit == "Mi") size *= 1024 * 1024;
      else if (unit == "Gi") size *= 1024 * 1024 * 1024;

      if (download->GetTotalSize() <= 0) {
        download->SetTotalSize(static_cast<int64_t>(size));
      }

      // Calculate downloaded size from percentage
      int64_t downloaded = static_cast<int64_t>(size * progress / 100.0);
      download->SetDownloadedSize(downloaded);
    }

    // Extract speed
    std::regex speedRegex(R"(at\s+(\d+\.?\d*)(Ki|Mi|Gi)?B/s)");
    if (std::regex_search(line, match, speedRegex)) {
      double speed = std::stod(match[1].str());
      std::string unit = match[2].str();
      if (unit == "Ki") speed *= 1024;
      else if (unit == "Mi") speed *= 1024 * 1024;
      else if (unit == "Gi") speed *= 1024 * 1024 * 1024;

      download->SetSpeed(speed);
    }
  }

  // Handle errors
  if (line.find("ERROR:") != std::string::npos) {
    size_t errorPos = line.find("ERROR:") + 6;
    std::string error = line.substr(errorPos);
    // Trim leading whitespace
    size_t start = error.find_first_not_of(" \t");
    if (start != std::string::npos) {
      error = error.substr(start);
    }
    download->SetErrorMessage(error);
    std::cerr << "[YtDlpManager] Error: " << error << std::endl;
  }

  // Handle warnings
  if (line.find("WARNING:") != std::string::npos) {
    std::cerr << "[YtDlpManager] " << line << std::endl;
  }

  // Handle video info
  if (line.find("[info]") != std::string::npos) {
    std::cout << "[YtDlpManager] " << line << std::endl;
  }

  // Log all output for debugging
  if (!line.empty() && line[0] != '[') {
    std::cout << "[YtDlpManager] Output: " << line << std::endl;
  }
}

void YtDlpManager::PauseDownload(int downloadId) {
  KillProcess(downloadId);
}

void YtDlpManager::ResumeDownload(std::shared_ptr<Download> download) {
  if (!download) return;

  // yt-dlp with -c flag will automatically resume
  StartDownload(download);
}

void YtDlpManager::CancelDownload(int downloadId) {
  KillProcess(downloadId);
}

bool YtDlpManager::WaitForDownloadFinish(int downloadId, int timeoutMs) {
  std::lock_guard<std::mutex> lock(m_tasksMutex);
  auto it = m_downloadTasks.find(downloadId);
  if (it == m_downloadTasks.end()) {
    return true;
  }

  if (!it->second.valid()) {
    return true;
  }

  return it->second.wait_for(std::chrono::milliseconds(timeoutMs)) !=
         std::future_status::timeout;
}

void YtDlpManager::KillProcess(int downloadId) {
  std::lock_guard<std::mutex> lock(m_processMutex);
  auto it = m_runningProcesses.find(downloadId);
  if (it != m_runningProcesses.end() && it->second) {
    TerminateProcess(it->second, 1);
    CloseHandle(it->second);
    m_runningProcesses.erase(it);
  }
}
