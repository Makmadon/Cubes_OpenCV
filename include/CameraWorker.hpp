#pragma once

#include "ThreadSafeFrame.hpp"
#include "Types.hpp"
#include <opencv2/videoio.hpp>
#include <atomic>
#include <thread>

namespace cubetracker {

/**
 * @brief Hilo dedicado exclusivamente a la captura continua de la cámara
 *        (Producer en la arquitectura multihilo).
 */
class CameraWorker {
public:
    CameraWorker(const AppConfig& config, LatestFrameBuffer& frame_buffer, std::atomic<bool>& stop_flag);
    ~CameraWorker();

    CameraWorker(const CameraWorker&) = delete;
    CameraWorker& operator=(const CameraWorker&) = delete;

    void start();
    void join();

private:
    void captureLoop();
    void runDemoGeneratorLoop();
    bool openCamera(cv::VideoCapture& cap);

    AppConfig config_;
    LatestFrameBuffer& frame_buffer_;
    std::atomic<bool>& stop_flag_;
    std::thread worker_thread_;
};

} // namespace cubetracker
