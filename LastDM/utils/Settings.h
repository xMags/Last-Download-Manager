#pragma once

#include <string>
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
  wxString GetDownloadFolder() const { return m_downloadFolder; }
  void SetDownloadFolder(const wxString &folder) { m_downloadFolder = folder; }

  bool GetAutoStart() const { return m_autoStart; }
  void SetAutoStart(bool value) { m_autoStart = value; }

  bool GetMinimizeToTray() const { return m_minimizeToTray; }
  void SetMinimizeToTray(bool value) { m_minimizeToTray = value; }

  bool GetShowNotifications() const { return m_showNotifications; }
  void SetShowNotifications(bool value) { m_showNotifications = value; }

  // Connection settings
  int GetMaxConnections() const { return m_maxConnections; }
  void SetMaxConnections(int value) { m_maxConnections = value; }

  int GetMaxSimultaneousDownloads() const { return m_maxSimultaneousDownloads; }
  void SetMaxSimultaneousDownloads(int value) {
    m_maxSimultaneousDownloads = value;
  }

  int GetSpeedLimit() const { return m_speedLimit; }
  void SetSpeedLimit(int value) { m_speedLimit = value; }

  // Proxy settings
  bool GetUseProxy() const { return m_useProxy; }
  void SetUseProxy(bool value) { m_useProxy = value; }

  std::string GetProxyHost() const { return m_proxyHost; }
  void SetProxyHost(const std::string &value) { m_proxyHost = value; }

  int GetProxyPort() const { return m_proxyPort; }
  void SetProxyPort(int value) { m_proxyPort = value; }

private:
  Settings();
  ~Settings() = default;

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
