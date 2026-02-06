#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <condition_variable>

// Lightweight HTTP server for browser extension integration.
// Listens on localhost only (127.0.0.1) for security.
// Uses token-based authentication to prevent unauthorized download injection.
class HttpServer {
public:
  // Callback receives URL and optional referer (page URL for protected downloads)
  using UrlCallback = std::function<void(const std::string &url, const std::string &referer)>;
  using StatusCallback = std::function<std::string()>;  // Returns JSON status

  static HttpServer &GetInstance();

  // Disable copy
  HttpServer(const HttpServer &) = delete;
  HttpServer &operator=(const HttpServer &) = delete;

  // Start the server on the specified port
  bool Start(int port = 45678);

  // Stop the server
  void Stop();

  // Check if running
  bool IsRunning() const { return m_running.load(); }

  // Get the port
  int GetPort() const { return m_port; }

  // Get the authentication token (for browser extension setup)
  std::string GetAuthToken() const { return m_authToken; }

  // Set callback for when a URL is received
  void SetUrlCallback(UrlCallback callback);

  // Set callback for status requests (returns active downloads info)
  void SetStatusCallback(StatusCallback callback);

private:
  HttpServer();
  ~HttpServer() noexcept;

  void ServerLoop();
  void HandleClient(uintptr_t clientSocket);
  std::string ParseUrlFromRequest(const std::string &request);
  std::string ExtractJsonValue(const std::string &json, const std::string &key);
  std::string GenerateAuthToken();
  bool ValidateOrigin(const std::string &request);
  std::string ExtractHeader(const std::string &request, const std::string &headerName);

  std::atomic<bool> m_running;
  std::thread m_serverThread;
  uintptr_t m_listenSocket;
  int m_port;
  std::string m_authToken;  // Authentication token for API security

  std::mutex m_callbackMutex;
  UrlCallback m_urlCallback;
  StatusCallback m_statusCallback;

  // Track active client handler threads to ensure clean shutdown
  std::mutex m_clientThreadsMutex;
  std::atomic<int> m_activeClientCount{0};
  std::condition_variable m_clientsDoneCondition;

  // Maximum concurrent connections to prevent thread exhaustion
  static constexpr int MAX_CONCURRENT_CLIENTS = 16;
};
