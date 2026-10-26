#pragma once

#include "ThreadSafeFrame.hpp"
#include "Types.hpp"
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace cubetracker {

/**
 * @brief Servidor HTTP multihilo ligero sin dependencias externas.
 *        Expone:
 *        - GET /api/cubes  -> JSON en tiempo real con (x, y, color, angle) de cada cubo.
 *        - GET /api/stream -> Stream de video MJPEG con detecciones dibujadas.
 *        - GET /           -> Panel web en vivo para monitoreo desde navegador.
 */
class HttpApiServer {
public:
    HttpApiServer(uint16_t port, SharedDetectionState& shared_state, std::atomic<bool>& stop_flag);
    ~HttpApiServer();

    HttpApiServer(const HttpApiServer&) = delete;
    HttpApiServer& operator=(const HttpApiServer&) = delete;

    void start();
    void join();

    static std::string serializeSnapshotJson(const DetectionSnapshot& snapshot);

private:
    void serverLoop();
    void handleClient(intptr_t client_sock);

    uint16_t port_;
    SharedDetectionState& shared_state_;
    std::atomic<bool>& stop_flag_;
    std::atomic<int> active_connections_{0};
    std::thread server_thread_;
};

} // namespace cubetracker
