#pragma once

#include "../core/Download.h"
#include <memory>
#include <sqlite3.h>
#include <string>
#include <vector>

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

  // Transaction support for batch operations
  bool BeginTransaction();
  bool CommitTransaction();
  bool RollbackTransaction();

private:
  DatabaseManager();
  ~DatabaseManager();

  sqlite3 *m_db;
  std::string m_dbPath;

  bool CreateTables();
  bool ExecuteSQL(const std::string &sql);

  // Helper to parse a download row from SQLite statement (#7 - DRY)
  std::unique_ptr<Download> ParseDownloadFromStatement(sqlite3_stmt *stmt);
};
