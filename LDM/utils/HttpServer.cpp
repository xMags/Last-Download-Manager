#include "HttpServer.h"

#include <sstream>
#include <random>
#include <iomanip>
#include <algorithm>  // for std::transform
#include <iostream>

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
  // Generate authentication token on startup
  m_authToken = GenerateAuthToken();
}

std::string HttpServer::GenerateAuthToken() {
  // Generate a cryptographically random token using Windows CryptoAPI
  std::stringstream ss;

#ifdef _WIN32
  // Use BCryptGenRandom for secure random bytes (available in Vista+)
  unsigned char buffer[32];
  HMODULE bcrypt = LoadLibraryA("bcrypt.dll");
  if (bcrypt) {
    typedef LONG (WINAPI *BCryptGenRandomFunc)(void*, unsigned char*, ULONG, ULONG);
    BCryptGenRandomFunc genRandom = (BCryptGenRandomFunc)GetProcAddress(bcrypt, "BCryptGenRandom");
    if (genRandom && genRandom(NULL, buffer, sizeof(buffer), 0x00000002) == 0) {
      for (int i = 0; i < 32; ++i) {
        ss << std::hex << std::setfill('0') << std::setw(2) << (int)buffer[i];
      }
      FreeLibrary(bcrypt);
      return ss.str();
    }
    FreeLibrary(bcrypt);
  }
#endif

  // Fallback to random_device (still reasonably secure on Windows)
  std::random_device rd;
  std::mt19937_64 gen(rd());
  std::uniform_int_distribution<unsigned int> dist(0, 255);
  for (int i = 0; i < 32; ++i) {
    ss << std::hex << std::setfill('0') << std::setw(2) << dist(gen);
  }
  return ss.str();
}

std::string HttpServer::ExtractHeader(const std::string &request, const std::string &headerName) {
  std::string searchKey = headerName + ":";

  // Case-insensitive search without copying the entire request string
  // Search line by line to avoid full string lowercase copy
  size_t lineStart = 0;
  while (lineStart < request.length()) {
    size_t lineEnd = request.find("\r\n", lineStart);
    if (lineEnd == std::string::npos) {
      lineEnd = request.length();
    }

    // Check if this line starts with the header (case-insensitive)
    if (lineEnd - lineStart >= searchKey.length()) {
      bool match = true;
      for (size_t i = 0; i < searchKey.length() && match; ++i) {
        char c1 = request[lineStart + i];
        char c2 = searchKey[i];
        // Case-insensitive comparison
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) match = false;
      }

      if (match) {
        // Found the header, extract value
        size_t valueStart = lineStart + searchKey.length();
        while (valueStart < lineEnd && request[valueStart] == ' ') {
          valueStart++;
        }
        return request.substr(valueStart, lineEnd - valueStart);
      }
    }

    // Move to next line
    if (lineEnd == request.length()) break;
    lineStart = lineEnd + 2;  // Skip \r\n

    // Stop at end of headers (empty line)
    if (lineStart < request.length() &&
        (request[lineStart] == '\r' || request[lineStart] == '\n')) {
      break;
    }
  }

  return "";
}

bool HttpServer::ValidateOrigin(const std::string &request) {
  std::string origin = ExtractHeader(request, "Origin");

  // If no Origin header, this could be a same-origin request or non-browser client
  // For local API, we allow requests without Origin (from curl, local apps, etc.)
  if (origin.empty()) {
    return true;
  }

  // Allow localhost origins (browser extensions and local dev)
  if (origin.find("http://127.0.0.1") == 0 ||
      origin.find("http://localhost") == 0 ||
      origin.find("https://127.0.0.1") == 0 ||
      origin.find("https://localhost") == 0 ||
      origin.find("chrome-extension://") == 0 ||
      origin.find("moz-extension://") == 0 ||
      origin.find("extension://") == 0) {
    return true;
  }

  return false;
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

  std::cout << "[HttpServer] Started on port " << port << std::endl;
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

  // Wait for all client handler threads to finish
  // Use a longer timeout (30s) and poll to ensure clean shutdown
  {
    std::unique_lock<std::mutex> lock(m_clientThreadsMutex);
    int waitAttempts = 0;
    const int maxWaitAttempts = 60;  // 60 * 500ms = 30 seconds max
    while (m_activeClientCount.load() > 0 && waitAttempts < maxWaitAttempts) {
      m_clientsDoneCondition.wait_for(lock, std::chrono::milliseconds(500));
      waitAttempts++;
    }
    // If threads are still active after timeout, they will be orphaned
    // but m_running is false so they will exit on next check
  }
}

void HttpServer::SetUrlCallback(UrlCallback callback) {
  std::lock_guard<std::mutex> lock(m_callbackMutex);
  m_urlCallback = callback;
  std::cout << "[HttpServer] URL callback " << (callback ? "set" : "cleared") << std::endl;
}

void HttpServer::SetStatusCallback(StatusCallback callback) {
  std::lock_guard<std::mutex> lock(m_callbackMutex);
  m_statusCallback = callback;
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

    // Enforce connection limit to prevent thread exhaustion
    if (m_activeClientCount.load() >= MAX_CONCURRENT_CLIENTS) {
      // Too many connections - send 503 and close immediately
      const char* response = "HTTP/1.1 503 Service Unavailable\r\n"
                             "Content-Length: 0\r\n\r\n";
      send(clientSocket, response, (int)strlen(response), 0);
      closesocket(clientSocket);
      continue;
    }

    // Handle in a detached thread (simple approach for low traffic)
    std::thread(&HttpServer::HandleClient, this, (uintptr_t)clientSocket)
        .detach();
  }
}

void HttpServer::HandleClient(uintptr_t clientSocket) {
  // RAII guard to track active client count
  m_activeClientCount.fetch_add(1);
  struct ClientGuard {
    HttpServer* server;
    ~ClientGuard() {
      if (server->m_activeClientCount.fetch_sub(1) == 1) {
        std::lock_guard<std::mutex> lock(server->m_clientThreadsMutex);
        server->m_clientsDoneCondition.notify_all();
      }
    }
  } guard{this};

  SOCKET sock = (SOCKET)clientSocket;

  // Set receive timeout (5 seconds) to prevent blocking forever
  DWORD timeout = 5000;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));

  // Read request (headers + body)
  std::string request;
  request.reserve(8192);
  char buffer[8192] = {};
  int bytesRead = recv(sock, buffer, sizeof(buffer) - 1, 0);

  if (bytesRead <= 0) {
    closesocket(sock);
    return;
  }

  request.append(buffer, bytesRead);

  // Ensure headers are fully read before parsing
  const size_t maxHeaderBytes = 64 * 1024;
  while (request.find("\r\n\r\n") == std::string::npos &&
         request.size() < maxHeaderBytes) {
    int more = recv(sock, buffer, sizeof(buffer) - 1, 0);
    if (more <= 0) {
      break;
    }
    request.append(buffer, more);
  }

  // If there is a body, keep reading until Content-Length is satisfied
  size_t headerEnd = request.find("\r\n\r\n");
  if (headerEnd != std::string::npos) {
    std::string contentLengthHeader = ExtractHeader(request, "Content-Length");
    int contentLength = 0;
    if (!contentLengthHeader.empty()) {
      try {
        int parsed = std::stoi(contentLengthHeader);
        contentLength = parsed < 0 ? 0 : parsed;
      } catch (...) {
        contentLength = 0;
      }
    }

    if (contentLength > 0) {
      size_t bodyStart = headerEnd + 4;
      size_t currentBodySize = request.size() > bodyStart ? request.size() - bodyStart : 0;
      while (currentBodySize < static_cast<size_t>(contentLength)) {
        int more = recv(sock, buffer, sizeof(buffer) - 1, 0);
        if (more <= 0) {
          break;
        }
        request.append(buffer, more);
        currentBodySize += static_cast<size_t>(more);
      }
    }
  }

  // Validate Origin header to prevent CSRF attacks
  if (!ValidateOrigin(request)) {
    std::string body = "{\"status\":\"error\",\"message\":\"Invalid origin\"}";
    std::string response = "HTTP/1.1 403 Forbidden\r\n"
                           "Content-Type: application/json\r\n"
                           "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    send(sock, response.c_str(), (int)response.size(), 0);
    closesocket(sock);
    return;
  }

  // CORS headers - dynamically set based on request Origin
  std::string origin = ExtractHeader(request, "Origin");
  std::string corsOrigin = origin.empty() ? "http://127.0.0.1" : origin;
  std::string corsHeaders =
      "Access-Control-Allow-Origin: " + corsOrigin + "\r\n"
      "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
      "Access-Control-Allow-Headers: Content-Type, X-Auth-Token\r\n"
      "Vary: Origin\r\n";

  // Handle OPTIONS (CORS preflight)
  if (request.find("OPTIONS ") == 0) {
    std::string response = "HTTP/1.1 204 No Content\r\n" + corsHeaders +
                           "Content-Length: 0\r\n\r\n";
    send(sock, response.c_str(), (int)response.size(), 0);
    closesocket(sock);
    return;
  }

  // Handle GET /ping - public endpoint for extension to check if LDM is running
  if (request.find("GET /ping") != std::string::npos) {
    std::string body = "{\"status\":\"ok\",\"app\":\"LDM\",\"version\":\"2.0.0\"}";
    std::string response = "HTTP/1.1 200 OK\r\n" + corsHeaders +
                           "Content-Type: application/json\r\n"
                           "Content-Length: " +
                           std::to_string(body.size()) + "\r\n\r\n" + body;
    send(sock, response.c_str(), (int)response.size(), 0);
    closesocket(sock);
    return;
  }

  // Handle GET /token - returns the auth token (only accessible from localhost)
  // Note: We don't require Origin header here because browser extensions may not send it
  // Security is provided by binding to 127.0.0.1 only
  if (request.find("GET /token") != std::string::npos) {
    std::cout << "[HttpServer] GET /token request - returning token" << std::endl;
    std::string body = "{\"token\":\"" + m_authToken + "\"}";
    std::string response = "HTTP/1.1 200 OK\r\n" + corsHeaders +
                           "Content-Type: application/json\r\n"
                           "Content-Length: " +
                           std::to_string(body.size()) + "\r\n\r\n" + body;
    send(sock, response.c_str(), (int)response.size(), 0);
    closesocket(sock);
    return;
  }

  // Handle GET /status - returns active downloads and speeds (public endpoint)
  if (request.find("GET /status") != std::string::npos) {
    std::string body;
    {
      std::lock_guard<std::mutex> lock(m_callbackMutex);
      if (m_statusCallback) {
        body = m_statusCallback();
      } else {
        body = "{\"status\":\"ok\",\"activeDownloads\":0,\"totalSpeed\":0,\"downloads\":[]}";
      }
    }
    std::string response = "HTTP/1.1 200 OK\r\n" + corsHeaders +
                           "Content-Type: application/json\r\n"
                           "Content-Length: " +
                           std::to_string(body.size()) + "\r\n\r\n" + body;
    send(sock, response.c_str(), (int)response.size(), 0);
    closesocket(sock);
    return;
  }

  // Handle POST /download - REQUIRES authentication token
  if (request.find("POST /download") != std::string::npos) {
    std::cout << "[HttpServer] Received POST /download request" << std::endl;

    // Extract body first
    std::string requestBody;
    size_t bodyStart = request.find("\r\n\r\n");
    if (bodyStart != std::string::npos) {
      requestBody = request.substr(bodyStart + 4);
    }
    std::cout << "[HttpServer] Request body: " << requestBody << std::endl;

    // Verify auth token from header or body
    std::string headerToken = ExtractHeader(request, "X-Auth-Token");
    std::string bodyToken = ExtractJsonValue(requestBody, "token");

    std::cout << "[HttpServer] Auth check - headerToken: '" << headerToken
              << "', bodyToken: '" << bodyToken
              << "', expected: '" << m_authToken << "'" << std::endl;

    bool authenticated = (!headerToken.empty() && headerToken == m_authToken) ||
                         (!bodyToken.empty() && bodyToken == m_authToken);

    if (!authenticated) {
      std::cout << "[HttpServer] Authentication FAILED - returning 401" << std::endl;
      std::string body = "{\"status\":\"error\",\"message\":\"Authentication required. Get token from GET /token\"}";
      std::string response = "HTTP/1.1 401 Unauthorized\r\n" + corsHeaders +
                             "Content-Type: application/json\r\n"
                             "Content-Length: " +
                             std::to_string(body.size()) + "\r\n\r\n" + body;
      send(sock, response.c_str(), (int)response.size(), 0);
      closesocket(sock);
      return;
    }

    std::string url = ExtractJsonValue(requestBody, "url");
    std::string referer = ExtractJsonValue(requestBody, "referer");
    std::cout << "[HttpServer] Extracted URL: " << url << std::endl;
    if (!referer.empty()) {
      std::cout << "[HttpServer] Referer from extension: " << referer << std::endl;
    }

    std::string body;
    if (!url.empty()) {
      // Invoke callback on main thread via posted message
      // Check m_running to avoid calling callback during shutdown
      {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        std::cout << "[HttpServer] m_running=" << m_running.load()
                  << ", m_urlCallback=" << (m_urlCallback ? "set" : "null") << std::endl;
        if (m_running.load() && m_urlCallback) {
          std::cout << "[HttpServer] Calling URL callback..." << std::endl;
          m_urlCallback(url, referer);
          std::cout << "[HttpServer] Callback returned" << std::endl;
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
