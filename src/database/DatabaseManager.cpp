#include "DatabaseManager.h"
#include <ShlObj.h>
#include <iostream>

DatabaseManager &DatabaseManager::GetInstance() {
  static DatabaseManager instance;
  return instance;
}

DatabaseManager::DatabaseManager() : m_db(nullptr) {}

DatabaseManager::~DatabaseManager() { Close(); }

bool DatabaseManager::Initialize(const std::string &dbPath) {
  if (m_db) {
    Close();
  }

  // Determine database path
  if (dbPath.empty()) {
    char appDataPath[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0,
                                   appDataPath))) {
      m_dbPath = std::string(appDataPath) + "\\LastDM";
      CreateDirectoryA(m_dbPath.c_str(), NULL);
      m_dbPath += "\\downloads.db";
    } else {
      m_dbPath = "downloads.db";
    }
  } else {
    m_dbPath = dbPath;
  }

  // Open database
  int rc = sqlite3_open(m_dbPath.c_str(), &m_db);
  if (rc != SQLITE_OK) {
    std::cerr << "Cannot open database: " << sqlite3_errmsg(m_db) << std::endl;
    return false;
  }

  // Create tables
  return CreateTables();
}

void DatabaseManager::Close() {
  if (m_db) {
    sqlite3_close(m_db);
    m_db = nullptr;
  }
}

bool DatabaseManager::CreateTables() {
  // Downloads table
  const char *downloadsTableSQL = R"(
        CREATE TABLE IF NOT EXISTS downloads (
            id INTEGER PRIMARY KEY,
            url TEXT NOT NULL,
            filename TEXT NOT NULL,
            save_path TEXT NOT NULL,
            total_size INTEGER DEFAULT -1,
            downloaded_size INTEGER DEFAULT 0,
            status TEXT DEFAULT 'Queued',
            category TEXT DEFAULT 'All Downloads',
            description TEXT DEFAULT '',
            date_added TEXT,
            date_completed TEXT,
            error_message TEXT DEFAULT ''
        );
    )";

  if (!ExecuteSQL(downloadsTableSQL)) {
    return false;
  }

  // Categories table
  const char *categoriesTableSQL = R"(
        CREATE TABLE IF NOT EXISTS categories (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT UNIQUE NOT NULL,
            icon_path TEXT DEFAULT ''
        );
    )";

  if (!ExecuteSQL(categoriesTableSQL)) {
    return false;
  }

  // Settings table
  const char *settingsTableSQL = R"(
        CREATE TABLE IF NOT EXISTS settings (
            key TEXT PRIMARY KEY,
            value TEXT
        );
    )";

  if (!ExecuteSQL(settingsTableSQL)) {
    return false;
  }

  // Insert default categories
  const char *defaultCategoriesSQL = R"(
        INSERT OR IGNORE INTO categories (name) VALUES 
        ('All Downloads'),
        ('Compressed'),
        ('Documents'),
        ('Music'),
        ('Programs'),
        ('Video');
    )";

  ExecuteSQL(defaultCategoriesSQL);

  return true;
}

bool DatabaseManager::ExecuteSQL(const std::string &sql) {
  // Critical: Check if database is initialized
  if (!m_db) {
    std::cerr << "Database error: Database not initialized" << std::endl;
    return false;
  }

  char *errMsg = nullptr;
  int rc = sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, &errMsg);

  if (rc != SQLITE_OK) {
    std::cerr << "SQL error: " << (errMsg ? errMsg : "Unknown error")
              << std::endl;
    if (errMsg) {
      sqlite3_free(errMsg);
    }
    return false;
  }

  return true;
}

bool DatabaseManager::SaveDownload(const Download &download) {
  const char *sql = R"(
        INSERT OR REPLACE INTO downloads 
        (id, url, filename, save_path, total_size, downloaded_size, status, category, description, date_added)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, datetime('now'));
    )";

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK)
    return false;

  sqlite3_bind_int(stmt, 1, download.GetId());
  sqlite3_bind_text(stmt, 2, download.GetUrl().c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, download.GetFilename().c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, download.GetSavePath().c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 5, download.GetTotalSize());
  sqlite3_bind_int64(stmt, 6, download.GetDownloadedSize());
  sqlite3_bind_text(stmt, 7, download.GetStatusString().c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 8, download.GetCategory().c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 9, download.GetDescription().c_str(), -1,
                    SQLITE_TRANSIENT);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return rc == SQLITE_DONE;
}

bool DatabaseManager::UpdateDownload(const Download &download) {
  const char *sql = R"(
        UPDATE downloads SET 
        downloaded_size = ?,
        status = ?,
        error_message = ?
        WHERE id = ?;
    )";

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK)
    return false;

  sqlite3_bind_int64(stmt, 1, download.GetDownloadedSize());
  sqlite3_bind_text(stmt, 2, download.GetStatusString().c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, download.GetErrorMessage().c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 4, download.GetId());

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return rc == SQLITE_DONE;
}

bool DatabaseManager::DeleteDownload(int downloadId) {
  const char *sql = "DELETE FROM downloads WHERE id = ?;";

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK)
    return false;

  sqlite3_bind_int(stmt, 1, downloadId);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return rc == SQLITE_DONE;
}

std::unique_ptr<Download> DatabaseManager::LoadDownload(int downloadId) {
  const char *sql = "SELECT * FROM downloads WHERE id = ?;";

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK)
    return nullptr;

  sqlite3_bind_int(stmt, 1, downloadId);

  std::unique_ptr<Download> download;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    download = ParseDownloadFromStatement(stmt);
  }

  sqlite3_finalize(stmt);
  return download;
}

// Helper function to parse a download from SQLite statement row (#7 - DRY
// principle)
std::unique_ptr<Download>
DatabaseManager::ParseDownloadFromStatement(sqlite3_stmt *stmt) {
  int id = sqlite3_column_int(stmt, 0);
  const char *url =
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
  const char *savePath =
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));

  auto download =
      std::make_unique<Download>(id, url ? url : "", savePath ? savePath : "");

  const char *filename =
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
  if (filename)
    download->SetFilename(filename);

  download->SetTotalSize(sqlite3_column_int64(stmt, 4));
  download->SetDownloadedSize(sqlite3_column_int64(stmt, 5));

  const char *category =
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, 7));
  if (category)
    download->SetCategory(category);

  const char *description =
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, 8));
  if (description)
    download->SetDescription(description);

  // Restore status from database
  const char *statusStr =
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, 6));
  if (statusStr) {
    std::string status(statusStr);
    if (status == "Completed")
      download->SetStatus(DownloadStatus::Completed);
    else if (status == "Paused")
      download->SetStatus(DownloadStatus::Paused);
    else if (status == "Error")
      download->SetStatus(DownloadStatus::Error);
    else if (status == "Cancelled")
      download->SetStatus(DownloadStatus::Cancelled);
    // else remains as default Queued
  }

  // Restore error message if any
  const char *errorMsg =
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, 11));
  if (errorMsg)
    download->SetErrorMessage(errorMsg);

  return download;
}

std::vector<std::unique_ptr<Download>> DatabaseManager::LoadAllDownloads() {
  std::vector<std::unique_ptr<Download>> downloads;

  const char *sql = "SELECT * FROM downloads ORDER BY id DESC;";

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK)
    return downloads;

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    auto download = ParseDownloadFromStatement(stmt);
    if (download) {
      downloads.push_back(std::move(download));
    }
  }

  sqlite3_finalize(stmt);
  return downloads;
}

std::vector<std::string> DatabaseManager::GetCategories() {
  std::vector<std::string> categories;

  const char *sql = "SELECT name FROM categories ORDER BY id;";

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK)
    return categories;

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *name =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
    if (name)
      categories.push_back(name);
  }

  sqlite3_finalize(stmt);
  return categories;
}

bool DatabaseManager::AddCategory(const std::string &name) {
  const char *sql = "INSERT OR IGNORE INTO categories (name) VALUES (?);";

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK)
    return false;

  sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return rc == SQLITE_DONE;
}

bool DatabaseManager::DeleteCategory(const std::string &name) {
  const char *sql = "DELETE FROM categories WHERE name = ?;";

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK)
    return false;

  sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return rc == SQLITE_DONE;
}

std::string DatabaseManager::GetSetting(const std::string &key,
                                        const std::string &defaultValue) {
  const char *sql = "SELECT value FROM settings WHERE key = ?;";

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK)
    return defaultValue;

  sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);

  std::string value = defaultValue;

  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *val =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
    if (val)
      value = val;
  }

  sqlite3_finalize(stmt);
  return value;
}

bool DatabaseManager::SetSetting(const std::string &key,
                                 const std::string &value) {
  const char *sql =
      "INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?);";

  sqlite3_stmt *stmt;
  int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);
  if (rc != SQLITE_OK)
    return false;

  sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return rc == SQLITE_DONE;
}

bool DatabaseManager::ClearHistory() {
  return ExecuteSQL("DELETE FROM downloads;");
}

bool DatabaseManager::ClearCompleted() {
  return ExecuteSQL("DELETE FROM downloads WHERE status = 'Completed';");
}

bool DatabaseManager::BeginTransaction() {
  return ExecuteSQL("BEGIN TRANSACTION;");
}

bool DatabaseManager::CommitTransaction() { return ExecuteSQL("COMMIT;"); }

bool DatabaseManager::RollbackTransaction() { return ExecuteSQL("ROLLBACK;"); }
