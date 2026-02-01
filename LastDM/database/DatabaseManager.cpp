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

void DatabaseManager::Close() { SaveDatabase(); }

bool DatabaseManager::LoadDatabase() {
  wxXmlDocument doc;
  if (!doc.Load(m_dbPath))
    return false;

  m_data.downloads.clear();
  m_data.categories.clear();
  m_data.settings.clear();

  wxXmlNode *root = doc.GetRoot();
  if (!root || root->GetName() != "LastDM")
    return false;

  wxXmlNode *child = root->GetChildren();
  while (child) {
    if (child->GetName() == "Downloads") {
      wxXmlNode *downloadNode = child->GetChildren();
      while (downloadNode) {
        if (downloadNode->GetName() == "Download") {
          int id =
              std::stoi(downloadNode->GetAttribute("id", "0").ToStdString());
          std::string url = downloadNode->GetAttribute("url", "").ToStdString();
          std::string savePath =
              downloadNode->GetAttribute("save_path", "").ToStdString();

          auto download = std::make_shared<Download>(id, url, savePath);
          download->SetFilename(
              downloadNode->GetAttribute("filename", "").ToStdString());
          download->SetTotalSize(std::stoll(
              downloadNode->GetAttribute("total_size", "0").ToStdString()));
          download->SetDownloadedSize(
              std::stoll(downloadNode->GetAttribute("downloaded_size", "0")
                             .ToStdString()));
          download->SetCategory(
              downloadNode->GetAttribute("category", "").ToStdString());
          download->SetDescription(
              downloadNode->GetAttribute("description", "").ToStdString());

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
          else
            download->SetStatus(DownloadStatus::Queued);

          download->SetErrorMessage(
              downloadNode->GetAttribute("error_message", "").ToStdString());

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
  wxXmlNode *root = new wxXmlNode(NULL, wxXML_ELEMENT_NODE, "LastDM");
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
    node->AddAttribute("error_message", download->GetErrorMessage());
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

  return doc.Save(m_dbPath);
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
    newDownload->SetTotalSize(download.GetTotalSize());
    newDownload->SetDownloadedSize(download.GetDownloadedSize());
    newDownload->SetStatus(download.GetStatus());
    m_data.downloads.push_back(newDownload);
  }
  return SaveDatabase();
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
    return SaveDatabase();
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
    return SaveDatabase();
  }
  return true;
}

bool DatabaseManager::DeleteCategory(const std::string &name) {
  std::lock_guard<std::mutex> lock(m_mutex);
  auto it =
      std::remove(m_data.categories.begin(), m_data.categories.end(), name);
  if (it != m_data.categories.end()) {
    m_data.categories.erase(it, m_data.categories.end());
    return SaveDatabase();
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
  return SaveDatabase();
}

bool DatabaseManager::ClearHistory() {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_data.downloads.clear();
  return SaveDatabase();
}

bool DatabaseManager::ClearCompleted() {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_data.downloads.erase(
      std::remove_if(m_data.downloads.begin(), m_data.downloads.end(),
                     [](const std::shared_ptr<Download> &d) {
                       return d->GetStatus() == DownloadStatus::Completed;
                     }),
      m_data.downloads.end());
  return SaveDatabase();
}

void DatabaseManager::CreateDefaultCategories() {
  m_data.categories = {"All Downloads", "Compressed", "Documents",
                       "Music",         "Programs",   "Video"};
}
