#include "CameraWorker.hpp"
#include <opencv2/imgproc.hpp>
#include <chrono>
#include <cmath>
#include <iostream>

namespace cubetracker {

CameraWorker::CameraWorker(const AppConfig& config, LatestFrameBuffer& frame_buffer, std::atomic<bool>& stop_flag)
    : config_(config), frame_buffer_(frame_buffer), stop_flag_(stop_flag) {}

CameraWorker::~CameraWorker() {
    join();
}

void CameraWorker::start() {
    if (config_.demo_mode) {
        worker_thread_ = std::thread(&CameraWorker::runDemoGeneratorLoop, this);
    } else {
        worker_thread_ = std::thread(&CameraWorker::captureLoop, this);
    }
}

void CameraWorker::join() {
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

bool CameraWorker::openCamera(cv::VideoCapture& cap) {
    if (!config_.video_source.empty()) {
        std::cout << "[CameraThread] Abriendo fuente de video IP / archivo: " << config_.video_source << std::endl;

        const bool is_network_stream = (config_.video_source.rfind("rtsp://", 0) == 0 ||
                                        config_.video_source.rfind("http://", 0) == 0 ||
                                        config_.video_source.rfind("https://", 0) == 0);
        if (is_network_stream) {
            // Optimizar OpenCV FFmpeg para transmisiones IP: conexion TCP estable y sin buffering
#ifdef _WIN32
            _putenv("OPENCV_FFMPEG_CAPTURE_OPTIONS=rtsp_transport;tcp|fflags;nobuffer|max_delay;500000");
#else
            setenv("OPENCV_FFMPEG_CAPTURE_OPTIONS", "rtsp_transport;tcp|fflags;nobuffer|max_delay;500000", 0);
#endif
        }

        cap.open(config_.video_source, cv::CAP_ANY);
    } else {
        std::cout << "[CameraThread] Abriendo camara indice " << config_.camera_index << "..." << std::endl;
#ifdef __linux__
        // En Raspberry Pi / Linux, V4L2 ofrece menor latencia y control directo del buffer
        if (!cap.open(config_.camera_index, cv::CAP_V4L2)) {
            cap.open(config_.camera_index, cv::CAP_ANY);
        }
#else
        cap.open(config_.camera_index, cv::CAP_ANY);
#endif
    }

    if (!cap.isOpened()) {
        return false;
    }

    // Configurar buffer de tamaño 1 para evitar retrasos de frames acumulados en Raspberry Pi
    cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
    cap.set(cv::CAP_PROP_FRAME_WIDTH, config_.frame_width);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, config_.frame_height);
    cap.set(cv::CAP_PROP_FPS, config_.target_fps);

    return true;
}

void CameraWorker::captureLoop() {
    cv::VideoCapture cap;
    if (!openCamera(cap)) {
        std::cerr << "[CameraThread] ERROR: No se pudo abrir la camara (indice " << config_.camera_index
                  << "). Puedes usar --demo para probar sin camara fisica." << std::endl;
        stop_flag_.store(true, std::memory_order_relaxed);
        frame_buffer_.notifyStop();
        return;
    }

    const int actual_w = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
    const int actual_h = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
    std::cout << "[CameraThread] Camara activa a " << actual_w << "x" << actual_h << " px." << std::endl;

    cv::Mat frame;
    int consecutive_failures = 0;

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        if (!cap.read(frame) || frame.empty()) {
            ++consecutive_failures;
            if (consecutive_failures > 30) {
                std::cerr << "[CameraThread] Advertencia: Fallo de lectura de camara. Reintentando..." << std::endl;
                cap.release();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                openCamera(cap);
                consecutive_failures = 0;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            continue;
        }

        consecutive_failures = 0;
        frame_buffer_.publish(frame);
    }

    cap.release();
    std::cout << "[CameraThread] Hilo de captura finalizado." << std::endl;
}

void CameraWorker::runDemoGeneratorLoop() {
    std::cout << "[CameraThread] Modo DEMO activo: Generando cubos sinteticos en rotacion a "
              << config_.frame_width << "x" << config_.frame_height << " px." << std::endl;

    const auto frame_duration = std::chrono::milliseconds(1000 / std::max(1, config_.target_fps));
    float tick = 0.0f;

    struct DemoCube {
        cv::Point2f base_pos;
        float size;
        float base_angle;
        float speed_deg;
        cv::Scalar bgr;
    };

    const float w = static_cast<float>(config_.frame_width);
    const float h = static_cast<float>(config_.frame_height);

    std::vector<DemoCube> demo_cubes = {
        {{w * 0.25f, h * 0.35f}, 90.0f, 0.0f,   25.0f,  cv::Scalar(20, 20, 230)},   // Rojo
        {{w * 0.72f, h * 0.35f}, 85.0f, 15.0f, -18.0f,  cv::Scalar(30, 200, 30)},   // Verde
        {{w * 0.28f, h * 0.72f}, 95.0f, -30.0f, 12.0f,  cv::Scalar(230, 110, 20)},  // Azul
        {{w * 0.70f, h * 0.70f}, 80.0f, 10.0f,  32.0f,  cv::Scalar(15, 225, 240)}   // Amarillo
    };

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        const auto start_time = std::chrono::steady_clock::now();

        cv::Mat frame(config_.frame_height, config_.frame_width, CV_8UC3, cv::Scalar(35, 35, 38));

        for (const auto& dc : demo_cubes) {
            const float current_angle = dc.base_angle + tick * dc.speed_deg;
            const cv::Point2f center(
                dc.base_pos.x + 20.0f * std::sin(tick * 0.8f),
                dc.base_pos.y + 15.0f * std::cos(tick * 0.8f)
            );

            cv::RotatedRect rrect(center, cv::Size2f(dc.size, dc.size), current_angle);
            cv::Point2f pts2f[4];
            rrect.points(pts2f);

            std::vector<cv::Point> poly(4);
            for (int i = 0; i < 4; ++i) {
                poly[i] = cv::Point(static_cast<int>(std::round(pts2f[i].x)),
                                    static_cast<int>(std::round(pts2f[i].y)));
            }
            cv::fillConvexPoly(frame, poly, dc.bgr, cv::LINE_AA);
        }

        frame_buffer_.publish(frame);
        tick += 1.0f / static_cast<float>( std::max(1, config_.target_fps) );

        const auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed < frame_duration) {
            std::this_thread::sleep_for(frame_duration - elapsed);
        }
    }

    std::cout << "[CameraThread] Hilo generador DEMO finalizado." << std::endl;
}

} // namespace cubetracker
