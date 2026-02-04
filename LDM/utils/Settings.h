#pragma once

#include <mutex>
#include <string>
#include <algorithm>
#include <wx/stdpaths.h>

class Settings {
public:
  static Settings &GetInstance();

  // Disable copy
  Settings(const Settings &) = delete;
  Settings &operator=(const Settings &) = delete;

  // Load/Save
  void Load();
  void Save();

  // General settings
  wxString GetDownloadFolder() const { 
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_downloadFolder; 
  }
  void SetDownloadFolder(const wxString &folder) { 
    std::lock_guard<std::mutex> lock(m_mutex);
    m_downloadFolder = folder; 
  }

  bool GetAutoStart() const { 
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_autoStart; 
  }
  void SetAutoStart(bool value) { 
    std::lock_guard<std::mutex> lock(m_mutex);
    m_autoStart = value; 
  }

  bool GetMinimizeToTray() const { 
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_minimizeToTray; 
  }
  void SetMinimizeToTray(bool value) { 
    std::lock_guard<std::mutex> lock(m_mutex);
    m_minimizeToTray = value; 
  }

  bool GetShowNotifications() const { 
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_showNotifications; 
  }
  void SetShowNotifications(bool value) { 
    std::lock_guard<std::mutex> lock(m_mutex);
    m_showNotifications = value; 
  }

  // Connection settings
  int GetMaxConnections() const { 
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_maxConnections; 
  }
  void SetMaxConnections(int value) { 
    std::lock_guard<std::mutex> lock(m_mutex);
    m_maxConnections = std::max(1, value); 
  }

  int GetMaxSimultaneousDownloads() const { 
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_maxSimultaneousDownloads; 
  }
  void SetMaxSimultaneousDownloads(int value) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_maxSimultaneousDownloads = std::max(1, value);
  }

  int GetSpeedLimit() const { 
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_speedLimit; 
  }
  void SetSpeedLimit(int value) { 
    std::lock_guard<std::mutex> lock(m_mutex);
    m_speedLimit = std::max(0, value); 
  }

  // Proxy settings
  bool GetUseProxy() const { 
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_useProxy; 
  }
  void SetUseProxy(bool value) { 
    std::lock_guard<std::mutex> lock(m_mutex);
    m_useProxy = value; 
  }

  std::string GetProxyHost() const { 
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_proxyHost; 
  }
  void SetProxyHost(const std::string &value) { 
    std::lock_guard<std::mutex> lock(m_mutex);
    m_proxyHost = value; 
  }

  int GetProxyPort() const { 
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_proxyPort; 
  }
  void SetProxyPort(int value) { 
    std::lock_guard<std::mutex> lock(m_mutex);
    m_proxyPort = std::clamp(value, 1, 65535); 
  }

private:
  Settings();
  ~Settings() = default;

  mutable std::mutex m_mutex;

  // General
  wxString m_downloadFolder;
  bool m_autoStart;
  bool m_minimizeToTray;
  bool m_showNotifications;

  // Connection
  int m_maxConnections;
  int m_maxSimultaneousDownloads;
  int m_speedLimit;

  // Proxy
  bool m_useProxy;
  std::string m_proxyHost;
  int m_proxyPort;
};
