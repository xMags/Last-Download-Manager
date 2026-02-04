#include "HttpServer.h"

#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <WS2tcpip.h>
#include <WinSock2.h>
#pragma comment(lib, "Ws2_32.lib")
#endif

HttpServer &HttpServer::GetInstance() {
  static HttpServer instance;
  return instance;
}

HttpServer::HttpServer()
    : m_running(false), m_listenSocket(INVALID_SOCKET), m_port(45678) {
#ifdef _WIN32
  WSADATA wsaData;
  WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
}

HttpServer::~HttpServer() noexcept {
  Stop();
#ifdef _WIN32
  WSACleanup();
#endif
}

bool HttpServer::Start(int port) {
  if (m_running.load())
    return true;

  m_port = port;

  // Create socket
  m_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (m_listenSocket == INVALID_SOCKET) {
    return false;
  }

  // Allow address reuse
  int opt = 1;
  setsockopt((SOCKET)m_listenSocket, SOL_SOCKET, SO_REUSEADDR, (char *)&opt,
             sizeof(opt));

  // Bind to localhost only (security: external connections blocked)
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons((u_short)port);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

  if (bind((SOCKET)m_listenSocket, (sockaddr *)&addr, sizeof(addr)) ==
      SOCKET_ERROR) {
    closesocket((SOCKET)m_listenSocket);
    m_listenSocket = INVALID_SOCKET;
    return false;
  }

  // Listen
  if (listen((SOCKET)m_listenSocket, SOMAXCONN) == SOCKET_ERROR) {
    closesocket((SOCKET)m_listenSocket);
    m_listenSocket = INVALID_SOCKET;
    return false;
  }

  m_running.store(true);
  m_serverThread = std::thread(&HttpServer::ServerLoop, this);

  return true;
}

void HttpServer::Stop() {
  if (!m_running.load())
    return;

  m_running.store(false);

  // Close listen socket to unblock accept()
  if (m_listenSocket != INVALID_SOCKET) {
    closesocket((SOCKET)m_listenSocket);
    m_listenSocket = INVALID_SOCKET;
  }

  if (m_serverThread.joinable()) {
    m_serverThread.join();
  }
}

void HttpServer::SetUrlCallback(UrlCallback callback) {
  std::lock_guard<std::mutex> lock(m_callbackMutex);
  m_urlCallback = callback;
}

void HttpServer::ServerLoop() {
  while (m_running.load()) {
    sockaddr_in clientAddr = {};
    int addrLen = sizeof(clientAddr);

    SOCKET clientSocket =
        accept((SOCKET)m_listenSocket, (sockaddr *)&clientAddr, &addrLen);

    if (clientSocket == INVALID_SOCKET) {
      continue;
    }

    // Handle in a detached thread (simple approach for low traffic)
    std::thread(&HttpServer::HandleClient, this, (uintptr_t)clientSocket)
        .detach();
  }
}

void HttpServer::HandleClient(uintptr_t clientSocket) {
  SOCKET sock = (SOCKET)clientSocket;

  // Set receive timeout (5 seconds) to prevent blocking forever
  DWORD timeout = 5000;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));

  // Read request (up to 8KB)
  char buffer[8192] = {};
  int bytesRead = recv(sock, buffer, sizeof(buffer) - 1, 0);

  if (bytesRead <= 0) {
    closesocket(sock);
    return;
  }

  std::string request(buffer, bytesRead);

  // CORS headers for browser extension
  std::string corsHeaders =
      "Access-Control-Allow-Origin: *\r\n"
      "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
      "Access-Control-Allow-Headers: Content-Type\r\n";

  // Handle OPTIONS (CORS preflight)
  if (request.find("OPTIONS ") == 0) {
    std::string response = "HTTP/1.1 204 No Content\r\n" + corsHeaders +
                           "Content-Length: 0\r\n\r\n";
    send(sock, response.c_str(), (int)response.size(), 0);
    closesocket(sock);
    return;
  }

  // Handle GET /ping
  if (request.find("GET /ping") != std::string::npos) {
    std::string body = "{\"status\":\"ok\",\"app\":\"LDM\",\"version\":\"1.0\"}";
    std::string response = "HTTP/1.1 200 OK\r\n" + corsHeaders +
                           "Content-Type: application/json\r\n"
                           "Content-Length: " +
                           std::to_string(body.size()) + "\r\n\r\n" + body;
    send(sock, response.c_str(), (int)response.size(), 0);
    closesocket(sock);
    return;
  }

  // Handle POST /download
  if (request.find("POST /download") != std::string::npos) {
    std::string url = ParseUrlFromRequest(request);

    std::string body;
    if (!url.empty()) {
      // Invoke callback on main thread via posted message
      {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        if (m_urlCallback) {
          m_urlCallback(url);
        }
      }
      body = "{\"status\":\"ok\",\"message\":\"Download added\"}";
    } else {
      body = "{\"status\":\"error\",\"message\":\"Missing url parameter\"}";
    }

    std::string response = "HTTP/1.1 200 OK\r\n" + corsHeaders +
                           "Content-Type: application/json\r\n"
                           "Content-Length: " +
                           std::to_string(body.size()) + "\r\n\r\n" + body;
    send(sock, response.c_str(), (int)response.size(), 0);
    closesocket(sock);
    return;
  }

  // 404 for unknown endpoints
  std::string body = "{\"status\":\"error\",\"message\":\"Not found\"}";
  std::string response = "HTTP/1.1 404 Not Found\r\n" + corsHeaders +
                         "Content-Type: application/json\r\n"
                         "Content-Length: " +
                         std::to_string(body.size()) + "\r\n\r\n" + body;
  send(sock, response.c_str(), (int)response.size(), 0);
  closesocket(sock);
}

std::string HttpServer::ParseUrlFromRequest(const std::string &request) {
  // Find the body (after \r\n\r\n)
  size_t bodyStart = request.find("\r\n\r\n");
  if (bodyStart == std::string::npos) {
    return "";
  }
  bodyStart += 4;

  std::string body = request.substr(bodyStart);
  return ExtractJsonValue(body, "url");
}

std::string HttpServer::ExtractJsonValue(const std::string &json,
                                         const std::string &key) {
  // Simple JSON parser for {"key": "value"} format
  std::string searchKey = "\"" + key + "\"";
  size_t keyPos = json.find(searchKey);
  if (keyPos == std::string::npos) {
    return "";
  }

  size_t colonPos = json.find(':', keyPos + searchKey.length());
  if (colonPos == std::string::npos) {
    return "";
  }

  // Find opening quote
  size_t startQuote = json.find('"', colonPos + 1);
  if (startQuote == std::string::npos) {
    return "";
  }

  // Find closing quote (handle escaped quotes)
  size_t endQuote = startQuote + 1;
  while (endQuote < json.length()) {
    if (json[endQuote] == '"' && (endQuote == 0 || json[endQuote - 1] != '\\')) {
      break;
    }
    endQuote++;
  }

  if (endQuote >= json.length()) {
    return "";
  }

  return json.substr(startQuote + 1, endQuote - startQuote - 1);
}
