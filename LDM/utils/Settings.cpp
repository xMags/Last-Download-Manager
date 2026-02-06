#include "Settings.h"
#include "../database/DatabaseManager.h"
#include <algorithm>
#include <wx/filename.h>

Settings &Settings::GetInstance() {
  static Settings instance;
  return instance;
}

Settings::Settings()
    : m_autoStart(true), m_minimizeToTray(true), m_showNotifications(true),
      m_maxConnections(8), m_maxSimultaneousDownloads(3), m_speedLimit(0),
      m_useProxy(false), m_proxyPort(8080) {
  // Set default download folder to Windows Downloads folder
  m_downloadFolder = wxStandardPaths::Get().GetUserDir(wxStandardPaths::Dir_Downloads);

  // NOTE: Load() is NOT called here to avoid initialization order issues
  // The main application should call Settings::GetInstance().Load() after
  // DatabaseManager::GetInstance().Initialize() has been called
}

void Settings::Load() {
  DatabaseManager &db = DatabaseManager::GetInstance();

  // NOTE: Caller must ensure database is initialized before calling Load()
  // Do not call db.Initialize() here to avoid recursive initialization issues

  std::lock_guard<std::mutex> lock(m_mutex);

  // Load general settings
  m_downloadFolder =
      db.GetSetting("download_folder", m_downloadFolder.ToStdString());
  m_autoStart = db.GetSetting("auto_start", "1") == "1";
  m_minimizeToTray = db.GetSetting("minimize_to_tray", "1") == "1";
  m_showNotifications = db.GetSetting("show_notifications", "1") == "1";

  // Load connection settings with validation
  try {
    m_maxConnections = std::max(1, std::stoi(db.GetSetting("max_connections", "8")));
    m_maxSimultaneousDownloads =
        std::max(1, std::stoi(db.GetSetting("max_simultaneous_downloads", "3")));
    m_speedLimit = std::max(0, std::stoi(db.GetSetting("speed_limit", "0")));
  } catch (...) {
    // Use defaults on parse error
  }

  // Load proxy settings
  m_useProxy = db.GetSetting("use_proxy", "0") == "1";
  m_proxyHost = db.GetSetting("proxy_host", "");
  try {
    m_proxyPort = std::clamp(std::stoi(db.GetSetting("proxy_port", "8080")), 1, 65535);
  } catch (...) {
    m_proxyPort = 8080;
  }
}

void Settings::Save() {
  DatabaseManager &db = DatabaseManager::GetInstance();

  std::lock_guard<std::mutex> lock(m_mutex);

  // Save general settings
  db.SetSetting("download_folder", m_downloadFolder.ToStdString());
  db.SetSetting("auto_start", m_autoStart ? "1" : "0");
  db.SetSetting("minimize_to_tray", m_minimizeToTray ? "1" : "0");
  db.SetSetting("show_notifications", m_showNotifications ? "1" : "0");

  // Save connection settings
  db.SetSetting("max_connections", std::to_string(m_maxConnections));
  db.SetSetting("max_simultaneous_downloads",
                std::to_string(m_maxSimultaneousDownloads));
  db.SetSetting("speed_limit", std::to_string(m_speedLimit));

  // Save proxy settings
  db.SetSetting("use_proxy", m_useProxy ? "1" : "0");
  db.SetSetting("proxy_host", m_proxyHost);
  db.SetSetting("proxy_port", std::to_string(m_proxyPort));
}
