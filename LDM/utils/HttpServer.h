#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

// Lightweight HTTP server for browser extension integration.
// Listens on localhost only (127.0.0.1) for security.
class HttpServer {
public:
  using UrlCallback = std::function<void(const std::string &url)>;

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

  // Set callback for when a URL is received
  void SetUrlCallback(UrlCallback callback);

private:
  HttpServer();
  ~HttpServer() noexcept;

  void ServerLoop();
  void HandleClient(uintptr_t clientSocket);
  std::string ParseUrlFromRequest(const std::string &request);
  std::string ExtractJsonValue(const std::string &json, const std::string &key);

  std::atomic<bool> m_running;
  std::thread m_serverThread;
  uintptr_t m_listenSocket;
  int m_port;

  std::mutex m_callbackMutex;
  UrlCallback m_urlCallback;
};
