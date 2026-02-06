#include "DatabaseManager.h"
#include <ShlObj.h>
#include <algorithm>
#include <iostream>
#include <wx/filename.h>
#include <wx/stdpaths.h>


DatabaseManager &DatabaseManager::GetInstance() {
  static DatabaseManager instance;
  return instance;
}

DatabaseManager::DatabaseManager() {}

DatabaseManager::~DatabaseManager() { Close(); }

bool DatabaseManager::Initialize(const std::string &dbPath) {
  std::lock_guard<std::mutex> lock(m_mutex);

  // Determine database path
  if (dbPath.empty()) {
    wxStandardPathsBase &stdPaths = wxStandardPaths::Get();
    wxString userDataDir = stdPaths.GetUserDataDir();

    if (!wxDirExists(userDataDir)) {
      wxFileName::Mkdir(userDataDir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    }

    m_dbPath = (userDataDir + "\\downloads.xml").ToStdString();
  } else {
    m_dbPath = dbPath;
  }

  if (!LoadDatabase()) {
    CreateDefaultCategories();
    SaveDatabase();
  }

  return true;
}

void DatabaseManager::Close() { 
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_dirty) {
    SaveDatabase();
    m_dirty = false;
  }
}

void DatabaseManager::Flush() {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_dirty) {
    SaveDatabase();
    m_dirty = false;
  }
}

bool DatabaseManager::LoadDatabase() {
  wxXmlDocument doc;
  if (!doc.Load(m_dbPath))
    return false;

  m_data.downloads.clear();
  m_data.categories.clear();
  m_data.settings.clear();

  wxXmlNode *root = doc.GetRoot();
  if (!root || root->GetName() != "LDM")
    return false;

  // Safe parsing helpers to avoid crashes on corrupted XML
  auto safeStoi = [](const std::string &s, int defaultVal = 0) -> int {
    try {
      return std::stoi(s);
    } catch (...) {
      return defaultVal;
    }
  };
  auto safeStoll = [](const std::string &s, int64_t defaultVal = 0) -> int64_t {
    try {
      return std::stoll(s);
    } catch (...) {
      return defaultVal;
    }
  };

  wxXmlNode *child = root->GetChildren();
  while (child) {
    if (child->GetName() == "Downloads") {
      wxXmlNode *downloadNode = child->GetChildren();
      while (downloadNode) {
        if (downloadNode->GetName() == "Download") {
          int id =
              safeStoi(downloadNode->GetAttribute("id", "0").ToStdString());
          std::string url = downloadNode->GetAttribute("url", "").ToStdString();
          std::string savePath =
              downloadNode->GetAttribute("save_path", "").ToStdString();

          auto download = std::make_shared<Download>(id, url, savePath);
          download->SetFilename(
              downloadNode->GetAttribute("filename", "").ToStdString());
          download->SetTotalSize(safeStoll(
              downloadNode->GetAttribute("total_size", "0").ToStdString()));
          download->SetDownloadedSize(
              safeStoll(downloadNode->GetAttribute("downloaded_size", "0")
                             .ToStdString()));
          download->SetCategory(
              downloadNode->GetAttribute("category", "").ToStdString());
          download->SetDescription(
              downloadNode->GetAttribute("description", "").ToStdString());
          download->SetReferer(
              downloadNode->GetAttribute("referer", "").ToStdString());

          std::string statusStr =
              downloadNode->GetAttribute("status", "Queued").ToStdString();
          if (statusStr == "Completed")
            download->SetStatus(DownloadStatus::Completed);
          else if (statusStr == "Paused")
            download->SetStatus(DownloadStatus::Paused);
          else if (statusStr == "Error")
            download->SetStatus(DownloadStatus::Error);
          else if (statusStr == "Cancelled")
            download->SetStatus(DownloadStatus::Cancelled);
          else if (statusStr == "Downloading")
            // App likely crashed mid-download - set to Paused for explicit resume
            download->SetStatus(DownloadStatus::Paused);
          else
            download->SetStatus(DownloadStatus::Queued);

          download->SetErrorMessage(
              downloadNode->GetAttribute("error_message", "").ToStdString());

          // Load yt-dlp flag
          std::string isYtDlp =
              downloadNode->GetAttribute("is_ytdlp", "0").ToStdString();
          download->SetYtDlpDownload(isYtDlp == "1");

          // Load chunk metadata if present
          std::vector<DownloadChunk> chunks;
          wxXmlNode *downloadChild = downloadNode->GetChildren();
          while (downloadChild) {
            if (downloadChild->GetName() == "Chunks") {
              wxXmlNode *chunkNode = downloadChild->GetChildren();
              while (chunkNode) {
                if (chunkNode->GetName() == "Chunk") {
                  int64_t start = safeStoll(
                      chunkNode->GetAttribute("start", "0").ToStdString());
                  int64_t end = safeStoll(
                      chunkNode->GetAttribute("end", "0").ToStdString());
                  DownloadChunk chunk(start, end);
                  chunk.currentByte = safeStoll(
                      chunkNode->GetAttribute("current", "0").ToStdString());
                  chunk.completed =
                      chunkNode->GetAttribute("completed", "0") == "1";
                  chunks.push_back(chunk);
                }
                chunkNode = chunkNode->GetNext();
              }
            }
            downloadChild = downloadChild->GetNext();
          }
          if (!chunks.empty()) {
            download->SetChunks(chunks);
          }

          m_data.downloads.push_back(download);
        }
        downloadNode = downloadNode->GetNext();
      }
    } else if (child->GetName() == "Categories") {
      wxXmlNode *catNode = child->GetChildren();
      while (catNode) {
        if (catNode->GetName() == "Category") {
          m_data.categories.push_back(
              catNode->GetAttribute("name", "").ToStdString());
        }
        catNode = catNode->GetNext();
      }
    } else if (child->GetName() == "Settings") {
      wxXmlNode *setNode = child->GetChildren();
      while (setNode) {
        if (setNode->GetName() == "Setting") {
          m_data.settings.push_back(
              {setNode->GetAttribute("key", "").ToStdString(),
               setNode->GetAttribute("value", "").ToStdString()});
        }
        setNode = setNode->GetNext();
      }
    }
    child = child->GetNext();
  }
  return true;
}

bool DatabaseManager::SaveDatabase() {
  wxXmlDocument doc;
  wxXmlNode *root = new wxXmlNode(NULL, wxXML_ELEMENT_NODE, "LDM");
  doc.SetRoot(root);

  // Downloads
  wxXmlNode *downloadsNode =
      new wxXmlNode(root, wxXML_ELEMENT_NODE, "Downloads");
  for (const auto &download : m_data.downloads) {
    wxXmlNode *node =
        new wxXmlNode(downloadsNode, wxXML_ELEMENT_NODE, "Download");
    node->AddAttribute("id", std::to_string(download->GetId()));
    node->AddAttribute("url", download->GetUrl());
    node->AddAttribute("filename", download->GetFilename());
    node->AddAttribute("save_path", download->GetSavePath());
    node->AddAttribute("total_size", std::to_string(download->GetTotalSize()));
    node->AddAttribute("downloaded_size",
                       std::to_string(download->GetDownloadedSize()));
    node->AddAttribute("status", download->GetStatusString());
    node->AddAttribute("category", download->GetCategory());
    node->AddAttribute("description", download->GetDescription());
    node->AddAttribute("referer", download->GetReferer());
    node->AddAttribute("error_message", download->GetErrorMessage());
    node->AddAttribute("is_ytdlp", download->IsYtDlpDownload() ? "1" : "0");

    auto chunks = download->GetChunksCopy();
    if (!chunks.empty()) {
      wxXmlNode *chunksNode = new wxXmlNode(node, wxXML_ELEMENT_NODE, "Chunks");
      for (const auto &chunk : chunks) {
        wxXmlNode *chunkNode =
            new wxXmlNode(chunksNode, wxXML_ELEMENT_NODE, "Chunk");
        chunkNode->AddAttribute("start", std::to_string(chunk.startByte));
        chunkNode->AddAttribute("end", std::to_string(chunk.endByte));
        chunkNode->AddAttribute("current", std::to_string(chunk.currentByte));
        chunkNode->AddAttribute("completed", chunk.completed ? "1" : "0");
      }
    }
  }

  // Categories
  wxXmlNode *categoriesNode =
      new wxXmlNode(root, wxXML_ELEMENT_NODE, "Categories");
  for (const auto &cat : m_data.categories) {
    wxXmlNode *node =
        new wxXmlNode(categoriesNode, wxXML_ELEMENT_NODE, "Category");
    node->AddAttribute("name", cat);
  }

  // Settings
  wxXmlNode *settingsNode = new wxXmlNode(root, wxXML_ELEMENT_NODE, "Settings");
  for (const auto &set : m_data.settings) {
    wxXmlNode *node =
        new wxXmlNode(settingsNode, wxXML_ELEMENT_NODE, "Setting");
    node->AddAttribute("key", set.first);
    node->AddAttribute("value", set.second);
  }

  // Atomic save: write to temp file, then use ReplaceFile for atomic replacement
  std::string tempPath = m_dbPath + ".tmp";
  if (!doc.Save(tempPath)) {
    return false;
  }

  // Use ReplaceFile API for atomic replacement (safer against race conditions)
  // ReplaceFile is atomic and handles concurrent access better than Delete+Rename
  if (ReplaceFileA(m_dbPath.c_str(), tempPath.c_str(), NULL,
                   REPLACEFILE_IGNORE_MERGE_ERRORS, NULL, NULL)) {
    return true;
  }

  // ReplaceFile fails if destination doesn't exist, fallback to MoveFile
  DWORD err = GetLastError();
  if (err == ERROR_FILE_NOT_FOUND) {
    // First save - just rename temp to target
    if (MoveFileA(tempPath.c_str(), m_dbPath.c_str())) {
      return true;
    }
  }

  // Final fallback: delete and rename (less safe but works)
  DeleteFileA(m_dbPath.c_str());
  if (!MoveFileA(tempPath.c_str(), m_dbPath.c_str())) {
    // If rename fails, try to restore from temp
    CopyFileA(tempPath.c_str(), m_dbPath.c_str(), FALSE);
    DeleteFileA(tempPath.c_str());
    return false;
  }

  return true;
}

bool DatabaseManager::SaveDownload(const Download &download) {
  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = std::find_if(m_data.downloads.begin(), m_data.downloads.end(),
                         [&](const std::shared_ptr<Download> &d) {
                           return d->GetId() == download.GetId();
                         });

  if (it != m_data.downloads.end()) {
    // Update existing (Clone/Update values)
    // Since we are using shared_ptrs in pure in-memory replacement,
    // we might not need to strictly copy if the UI is holding the same pointer.
    // But to be safe and match behavior:
    (*it)->SetStatus(download.GetStatus());
    (*it)->SetDownloadedSize(download.GetDownloadedSize());
    (*it)->SetErrorMessage(download.GetErrorMessage());
    (*it)->SetChunks(download.GetChunksCopy());
    (*it)->SetYtDlpDownload(download.IsYtDlpDownload());
    // Copy other fields if needed, but usually only status/progress changes
    // frequently.
  } else {
    // Add new (Deep copy to ensure persistence doesn't depend on UI object
    // lifecycle if different)
    auto newDownload = std::make_shared<Download>(
        download.GetId(), download.GetUrl(), download.GetSavePath());
    newDownload->SetFilename(download.GetFilename());
    newDownload->SetCategory(download.GetCategory());
    newDownload->SetDescription(download.GetDescription());
    newDownload->SetReferer(download.GetReferer());
    newDownload->SetTotalSize(download.GetTotalSize());
    newDownload->SetDownloadedSize(download.GetDownloadedSize());
    newDownload->SetStatus(download.GetStatus());
    newDownload->SetChunks(download.GetChunksCopy());
    newDownload->SetYtDlpDownload(download.IsYtDlpDownload());
    m_data.downloads.push_back(newDownload);
  }
  MarkDirty();
  return true;
}

bool DatabaseManager::SyncAllDownloads(
    const std::vector<std::shared_ptr<Download>> &downloads) {
  std::lock_guard<std::mutex> lock(m_mutex);

  m_data.downloads.clear();
  m_data.downloads.reserve(downloads.size());

  for (const auto &download : downloads) {
    if (!download) {
      continue;
    }

    auto copy = std::make_shared<Download>(download->GetId(),
                                           download->GetUrl(),
                                           download->GetSavePath());
    copy->SetFilename(download->GetFilename());
    copy->SetCategory(download->GetCategory());
    copy->SetDescription(download->GetDescription());
    copy->SetReferer(download->GetReferer());
    copy->SetTotalSize(download->GetTotalSize());
    copy->SetDownloadedSize(download->GetDownloadedSize());
    copy->SetStatus(download->GetStatus());
    copy->SetErrorMessage(download->GetErrorMessage());
    copy->SetChunks(download->GetChunksCopy());
    copy->SetYtDlpDownload(download->IsYtDlpDownload());

    m_data.downloads.push_back(copy);
  }

  MarkDirty();
  return true;
}

bool DatabaseManager::UpdateDownload(const Download &download) {
  return SaveDownload(download);
}

bool DatabaseManager::DeleteDownload(int downloadId) {
  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = std::remove_if(m_data.downloads.begin(), m_data.downloads.end(),
                           [&](const std::shared_ptr<Download> &d) {
                             return d->GetId() == downloadId;
                           });

  if (it != m_data.downloads.end()) {
    m_data.downloads.erase(it, m_data.downloads.end());
    MarkDirty();
    return true;
  }
  return false;
}

std::unique_ptr<Download> DatabaseManager::LoadDownload(int downloadId) {
  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = std::find_if(m_data.downloads.begin(), m_data.downloads.end(),
                         [&](const std::shared_ptr<Download> &d) {
                           return d->GetId() == downloadId;
                         });

  if (it != m_data.downloads.end()) {
    // Return a copy as expected by the interface
    auto d = *it;
    auto copy =
        std::make_unique<Download>(d->GetId(), d->GetUrl(), d->GetSavePath());
    copy->SetFilename(d->GetFilename());
    copy->SetCategory(d->GetCategory());
    copy->SetDescription(d->GetDescription());
    copy->SetTotalSize(d->GetTotalSize());
    copy->SetDownloadedSize(d->GetDownloadedSize());
    copy->SetStatus(d->GetStatus());
    copy->SetErrorMessage(d->GetErrorMessage());
    copy->SetChunks(d->GetChunksCopy());
    copy->SetYtDlpDownload(d->IsYtDlpDownload());
    return copy;
  }
  return nullptr;
}

std::vector<std::unique_ptr<Download>> DatabaseManager::LoadAllDownloads() {
  std::lock_guard<std::mutex> lock(m_mutex);
  std::vector<std::unique_ptr<Download>> result;
  for (const auto &d : m_data.downloads) {
    auto copy =
        std::make_unique<Download>(d->GetId(), d->GetUrl(), d->GetSavePath());
    copy->SetFilename(d->GetFilename());
    copy->SetCategory(d->GetCategory());
    copy->SetDescription(d->GetDescription());
    copy->SetTotalSize(d->GetTotalSize());
    copy->SetDownloadedSize(d->GetDownloadedSize());
    copy->SetStatus(d->GetStatus());
    copy->SetErrorMessage(d->GetErrorMessage());
    copy->SetChunks(d->GetChunksCopy());
    copy->SetYtDlpDownload(d->IsYtDlpDownload());
    result.push_back(std::move(copy));
  }
  return result;
}

std::vector<std::string> DatabaseManager::GetCategories() {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_data.categories;
}

bool DatabaseManager::AddCategory(const std::string &name) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (std::find(m_data.categories.begin(), m_data.categories.end(), name) ==
      m_data.categories.end()) {
    m_data.categories.push_back(name);
    MarkDirty();
  }
  return true;
}

bool DatabaseManager::DeleteCategory(const std::string &name) {
  std::lock_guard<std::mutex> lock(m_mutex);
  auto it =
      std::remove(m_data.categories.begin(), m_data.categories.end(), name);
  if (it != m_data.categories.end()) {
    m_data.categories.erase(it, m_data.categories.end());
    MarkDirty();
    return true;
  }
  return false;
}

std::string DatabaseManager::GetSetting(const std::string &key,
                                        const std::string &defaultValue) {
  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = std::find_if(m_data.settings.begin(), m_data.settings.end(),
                         [&](const std::pair<std::string, std::string> &s) {
                           return s.first == key;
                         });

  if (it != m_data.settings.end()) {
    return it->second;
  }
  return defaultValue;
}

bool DatabaseManager::SetSetting(const std::string &key,
                                 const std::string &value) {
  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = std::find_if(m_data.settings.begin(), m_data.settings.end(),
                         [&](const std::pair<std::string, std::string> &s) {
                           return s.first == key;
                         });

  if (it != m_data.settings.end()) {
    it->second = value;
  } else {
    m_data.settings.push_back({key, value});
  }
  MarkDirty();
  return true;
}

bool DatabaseManager::ClearHistory() {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_data.downloads.clear();
  MarkDirty();
  return true;
}

bool DatabaseManager::ClearCompleted() {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_data.downloads.erase(
      std::remove_if(m_data.downloads.begin(), m_data.downloads.end(),
                     [](const std::shared_ptr<Download> &d) {
                       return d->GetStatus() == DownloadStatus::Completed;
                     }),
      m_data.downloads.end());
  MarkDirty();
  return true;
}

void DatabaseManager::CreateDefaultCategories() {
  m_data.categories = {"All Downloads", "Compressed", "Documents",
                       "Images",        "Music",      "Programs",   "Video"};
}
