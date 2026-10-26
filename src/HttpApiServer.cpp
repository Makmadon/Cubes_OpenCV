#include "HttpApiServer.hpp"
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
inline void closeSocket(socket_t s) { closesocket(s); }
constexpr int kSendFlags = 0;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
inline void closeSocket(socket_t s) { close(s); }
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif
#endif

namespace cubetracker {

namespace {

bool sendAll(socket_t sock, const char* data, size_t length) {
    size_t total_sent = 0;
    while (total_sent < length) {
        const int sent = ::send(sock, data + total_sent, static_cast<int>(length - total_sent), kSendFlags);
        if (sent <= 0) {
            return false;
        }
        total_sent += static_cast<size_t>(sent);
    }
    return true;
}

const char* kDashboardHtml = R"HTML(<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>CubeVisionTracker - Raspberry Pi API</title>
<style>
  :root { --bg: #0f172a; --card: #1e293b; --text: #f8fafc; --muted: #94a3b8; --accent: #38bdf8; }
  body { margin: 0; font-family: system-ui, -apple-system, sans-serif; background: var(--bg); color: var(--text); padding: 20px; }
  h1 { margin-top: 0; font-size: 1.4rem; color: var(--accent); }
  .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(360px, 1fr)); gap: 20px; }
  .card { background: var(--card); border-radius: 10px; padding: 16px; box-shadow: 0 4px 12px rgba(0,0,0,0.3); }
  img.stream { width: 100%; border-radius: 8px; background: #000; display: block; }
  table { width: 100%; border-collapse: collapse; margin-top: 10px; font-size: 0.92rem; }
  th, td { text-align: left; padding: 8px 10px; border-bottom: 1px solid #334155; }
  th { color: var(--muted); font-weight: 600; }
  pre { background: #0b1120; padding: 12px; border-radius: 8px; overflow-x: auto; font-size: 0.82rem; color: #a5f3fc; margin-top: 12px; }
  .badge { display: inline-block; padding: 2px 8px; border-radius: 999px; font-weight: 600; font-size: 0.8rem; background: #334155; }
</style>
</head>
<body>
  <h1>CubeVisionTracker — Panel en Tiempo Real (OpenCV C++ Multihilo)</h1>
  <div class="grid">
    <div class="card">
      <strong>Vista en Vivo (/api/stream)</strong>
      <p style="color:var(--muted);font-size:0.85rem;margin:6px 0 12px;">Cajas rotadas (minAreaRect) y vector de inclinación respecto a la vertical (0° = erguido)</p>
      <img class="stream" src="/api/stream" alt="Stream OpenCV" />
    </div>
    <div class="card">
      <strong>Cubos Detectados — Endpoint: <code>GET /api/cubes</code></strong>
      <table>
        <thead>
          <tr><th>Color</th><th>X (px)</th><th>Y (px)</th><th>Ángulo Vert. (°)</th><th>Tamaño (px)</th></tr>
        </thead>
        <tbody id="cubes-body">
          <tr><td colspan="5" style="color:var(--muted);">Cargando datos...</td></tr>
        </tbody>
      </table>
      <pre id="raw-json">{}</pre>
    </div>
  </div>
<script>
async function pollCubes() {
  try {
    const res = await fetch('/api/cubes');
    const data = await res.json();
    document.getElementById('raw-json').textContent = JSON.stringify(data, null, 2);
    const tbody = document.getElementById('cubes-body');
    if (!data.cubes || data.cubes.length === 0) {
      tbody.innerHTML = '<tr><td colspan="5" style="color:var(--muted);">Ningún cubo en escena</td></tr>';
    } else {
      tbody.innerHTML = data.cubes.map(c => `
        <tr>
          <td><span class="badge">${c.color}</span></td>
          <td>${c.x.toFixed(1)}</td>
          <td>${c.y.toFixed(1)}</td>
          <td><strong>${c.angle.toFixed(1)}°</strong></td>
          <td>${ Math.round(c.width) }x${ Math.round(c.height) }</td>
        </tr>
      `).join('');
    }
  } catch (e) {}
}
setInterval(pollCubes, 150);
pollCubes();
</script>
</body>
</html>)HTML";

struct StreamClientGuard {
    SharedDetectionState& state;
    explicit StreamClientGuard(SharedDetectionState& s) : state(s) { state.incrementStreamClients(); }
    ~StreamClientGuard() { state.decrementStreamClients(); }
};

} // namespace

HttpApiServer::HttpApiServer(uint16_t port, SharedDetectionState& shared_state, std::atomic<bool>& stop_flag)
    : port_(port), shared_state_(shared_state), stop_flag_(stop_flag) {}

HttpApiServer::~HttpApiServer() {
    join();
}

void HttpApiServer::start() {
    server_thread_ = std::thread(&HttpApiServer::serverLoop, this);
}

void HttpApiServer::join() {
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
    while (active_connections_.load(std::memory_order_relaxed) > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

std::string HttpApiServer::serializeSnapshotJson(const DetectionSnapshot& snapshot) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);
    oss << "{"
        << "\"frame_id\":" << snapshot.frame_id << ","
        << "\"timestamp_ms\":" << snapshot.timestamp_ms << ","
        << "\"fps\":" << snapshot.fps << ","
        << "\"resolution\":{\"width\":" << snapshot.frame_width << ",\"height\":" << snapshot.frame_height << "},"
        << "\"count\":" << snapshot.cubes.size() << ","
        << "\"cubes\":[";

    for (size_t i = 0; i < snapshot.cubes.size(); ++i) {
        const auto& c = snapshot.cubes[i];
        if (i > 0) {
            oss << ",";
        }
        oss << "{"
            << "\"color\":\"" << c.color << "\","
            << "\"x\":" << c.x << ","
            << "\"y\":" << c.y << ","
            << "\"angle\":" << c.angle << ","
            << "\"width\":" << c.width << ","
            << "\"height\":" << c.height << ","
            << "\"area\":" << c.area
            << "}";
    }
    oss << "]}";
    return oss.str();
}

void HttpApiServer::serverLoop() {
#ifdef _WIN32
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        std::cerr << "[HttpServer] Error al inicializar Winsock2." << std::endl;
        return;
    }
#endif

    socket_t listen_sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == kInvalidSocket) {
        std::cerr << "[HttpServer] No se pudo crear el socket TCP." << std::endl;
#ifdef _WIN32
        WSACleanup();
#endif
        return;
    }

    int opt = 1;
    ::setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (::bind(listen_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[HttpServer] Error en bind() para el puerto " << port_
                  << ". Verifica que no este en uso." << std::endl;
        closeSocket(listen_sock);
#ifdef _WIN32
        WSACleanup();
#endif
        return;
    }

    if (::listen(listen_sock, 16) < 0) {
        std::cerr << "[HttpServer] Error en listen()." << std::endl;
        closeSocket(listen_sock);
#ifdef _WIN32
        WSACleanup();
#endif
        return;
    }

    std::cout << "[HttpServer] Servidor Web API escuchando en:" << std::endl
              << "  -> Panel Web : http://0.0.0.0:" << port_ << "/" << std::endl
              << "  -> API JSON  : http://0.0.0.0:" << port_ << "/api/cubes" << std::endl
              << "  -> Stream    : http://0.0.0.0:" << port_ << "/api/stream" << std::endl;

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(listen_sock, &read_fds);

        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 200000; // 200 ms timeout para comprobar stop_flag_

        const int ready = ::select(static_cast<int>(listen_sock + 1), &read_fds, nullptr, nullptr, &tv);
        if (ready <= 0) {
            continue;
        }

        sockaddr_in client_addr{};
#ifdef _WIN32
        int client_len = sizeof(client_addr);
#else
        socklen_t client_len = sizeof(client_addr);
#endif
        socket_t client_sock = ::accept(listen_sock, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_sock == kInvalidSocket) {
            continue;
        }

        if (active_connections_.load(std::memory_order_relaxed) >= 16) {
            closeSocket(client_sock);
            continue;
        }

        active_connections_.fetch_add(1, std::memory_order_relaxed);
        std::thread([this, client_sock]() {
            handleClient(static_cast<intptr_t>(client_sock));
            active_connections_.fetch_sub(1, std::memory_order_relaxed);
        }).detach();
    }

    closeSocket(listen_sock);
#ifdef _WIN32
    WSACleanup();
#endif
    std::cout << "[HttpServer] Servidor Web API finalizado." << std::endl;
}

void HttpApiServer::handleClient(intptr_t raw_sock) {
    socket_t client_sock = static_cast<socket_t>(raw_sock);

    char buffer[2048];
    const int received = ::recv(client_sock, buffer, sizeof(buffer) - 1, 0);
    if (received <= 0) {
        closeSocket(client_sock);
        return;
    }
    buffer[received] = '\0';

    std::istringstream req_stream(buffer);
    std::string method;
    std::string path;
    req_stream >> method >> path;

    // Eliminar query strings si existen (ej. /api/cubes?t=123)
    const size_t qpos = path.find('?');
    if (qpos != std::string::npos) {
        path = path.substr(0, qpos);
    }

    if (method == "OPTIONS") {
        const std::string resp =
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Connection: close\r\n\r\n";
        sendAll(client_sock, resp.c_str(), resp.size());
        closeSocket(client_sock);
        return;
    }

    if (path == "/api/cubes" || path == "/cubes") {
        const DetectionSnapshot snapshot = shared_state_.getSnapshot();
        const std::string body = serializeSnapshotJson(snapshot);

        std::ostringstream resp;
        resp << "HTTP/1.1 200 OK\r\n"
             << "Content-Type: application/json; charset=utf-8\r\n"
             << "Access-Control-Allow-Origin: *\r\n"
             << "Cache-Control: no-store, no-cache, must-revalidate\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Connection: close\r\n\r\n"
             << body;

        const std::string full_resp = resp.str();
        sendAll(client_sock, full_resp.c_str(), full_resp.size());
    } else if (path == "/api/health") {
        const std::string body = "{\"status\":\"ok\"}";
        std::ostringstream resp;
        resp << "HTTP/1.1 200 OK\r\n"
             << "Content-Type: application/json\r\n"
             << "Access-Control-Allow-Origin: *\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Connection: close\r\n\r\n"
             << body;
        const std::string full_resp = resp.str();
        sendAll(client_sock, full_resp.c_str(), full_resp.size());
    } else if (path == "/api/stream" || path == "/stream") {
        StreamClientGuard guard(shared_state_);
        const std::string header =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Cache-Control: no-store, no-cache, must-revalidate\r\n"
            "Connection: close\r\n\r\n";

        if (!sendAll(client_sock, header.c_str(), header.size())) {
            closeSocket(client_sock);
            return;
        }

        uint64_t last_frame_id = 0;
        std::vector<uchar> jpeg_data;

        while (!stop_flag_.load(std::memory_order_relaxed)) {
            if (!shared_state_.waitForNextJpeg(last_frame_id, jpeg_data, stop_flag_)) {
                break;
            }

            std::ostringstream part_header;
            part_header << "--frame\r\n"
                        << "Content-Type: image/jpeg\r\n"
                        << "Content-Length: " << jpeg_data.size() << "\r\n\r\n";
            const std::string part_str = part_header.str();

            if (!sendAll(client_sock, part_str.c_str(), part_str.size()) ||
                !sendAll(client_sock, reinterpret_cast<const char*>(jpeg_data.data()), jpeg_data.size()) ||
                !sendAll(client_sock, "\r\n", 2)) {
                break;
            }
        }
    } else if (path == "/" || path == "/index.html") {
        const size_t html_len = std::strlen(kDashboardHtml);
        std::ostringstream resp;
        resp << "HTTP/1.1 200 OK\r\n"
             << "Content-Type: text/html; charset=utf-8\r\n"
             << "Content-Length: " << html_len << "\r\n"
             << "Connection: close\r\n\r\n";
        const std::string hdr = resp.str();
        sendAll(client_sock, hdr.c_str(), hdr.size());
        sendAll(client_sock, kDashboardHtml, html_len);
    } else {
        const std::string body = "{\"error\":\"Ruta no encontrada. Usa /api/cubes o /api/stream\"}";
        std::ostringstream resp;
        resp << "HTTP/1.1 404 Not Found\r\n"
             << "Content-Type: application/json\r\n"
             << "Access-Control-Allow-Origin: *\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Connection: close\r\n\r\n"
             << body;
        const std::string full_resp = resp.str();
        sendAll(client_sock, full_resp.c_str(), full_resp.size());
    }

    closeSocket(client_sock);
}

} // namespace cubetracker
