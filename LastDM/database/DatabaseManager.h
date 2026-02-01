#pragma once

#include "../core/Download.h"
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <wx/xml/xml.h>


class DatabaseManager {
public:
  static DatabaseManager &GetInstance();

  // Disable copy
  DatabaseManager(const DatabaseManager &) = delete;
  DatabaseManager &operator=(const DatabaseManager &) = delete;

  // Database operations
  bool Initialize(const std::string &dbPath = "");
  void Close();

  // Download CRUD operations
  bool SaveDownload(const Download &download);
  bool UpdateDownload(const Download &download);
  bool DeleteDownload(int downloadId);
  std::unique_ptr<Download> LoadDownload(int downloadId);
  std::vector<std::unique_ptr<Download>> LoadAllDownloads();

  // Category operations
  std::vector<std::string> GetCategories();
  bool AddCategory(const std::string &name);
  bool DeleteCategory(const std::string &name);

  // Settings operations
  std::string GetSetting(const std::string &key,
                         const std::string &defaultValue = "");
  bool SetSetting(const std::string &key, const std::string &value);

  // Cleanup
  bool ClearHistory();
  bool ClearCompleted();

  // Transaction support (No-op in XML version, kept for API compatibility)
  bool BeginTransaction() { return true; }
  bool CommitTransaction() { return SaveDatabase(); }
  bool RollbackTransaction() { return true; }

private:
  DatabaseManager();
  ~DatabaseManager();

  std::string m_dbPath;
  std::mutex m_mutex;

  // In-memory data
  struct AppData {
    std::vector<std::shared_ptr<Download>> downloads;
    std::vector<std::string> categories;
    std::vector<std::pair<std::string, std::string>> settings;
  } m_data;

  bool LoadDatabase();
  bool SaveDatabase();

  // Helpers
  void CreateDefaultCategories();
};
