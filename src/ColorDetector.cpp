#include "ColorDetector.hpp"
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace cubetracker {

ColorDetector::ColorDetector(const AppConfig& config,
                             LatestFrameBuffer& frame_buffer,
                             SharedDetectionState& shared_state,
                             std::atomic<bool>& stop_flag)
    : config_(config),
      frame_buffer_(frame_buffer),
      shared_state_(shared_state),
      stop_flag_(stop_flag),
      color_profiles_(defaultColorProfiles()),
      morph_kernel_(cv::getStructuringElement(cv::MORPH_RECT, cv::Size(5, 5))) {}

ColorDetector::~ColorDetector() {
    join();
}

void ColorDetector::start() {
    worker_thread_ = std::thread(&ColorDetector::processingLoop, this);
}

void ColorDetector::join() {
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

std::vector<ColorProfile> ColorDetector::defaultColorProfiles() {
    return {
        {
            "Rojo",
            cv::Scalar(40, 40, 255),
            {
                // El rojo en HSV de OpenCV (H: 0..180) abarca los extremos inferior y superior
                {cv::Scalar(0, 110, 80), cv::Scalar(10, 255, 255)},
                {cv::Scalar(165, 110, 80), cv::Scalar(180, 255, 255)}
            }
        },
        {
            "Verde",
            cv::Scalar(40, 230, 40),
            {
                {cv::Scalar(36, 90, 70), cv::Scalar(86, 255, 255)}
            }
        },
        {
            "Azul",
            cv::Scalar(255, 140, 30),
            {
                {cv::Scalar(95, 110, 70), cv::Scalar(135, 255, 255)}
            }
        },
        {
            "Amarillo",
            cv::Scalar(0, 230, 255),
            {
                {cv::Scalar(18, 110, 90), cv::Scalar(34, 255, 255)}
            }
        }
    };
}

float ColorDetector::computeStableCubeAngleDeg(const cv::Point2f pts[4]) {
    // Evaluamos las dos aristas perpendiculares adyacentes del rectangulo rotado
    cv::Point2f e1 = pts[1] - pts[0];
    cv::Point2f e2 = pts[2] - pts[1];

    // Orientar ambos vectores hacia la parte superior de la imagen (y <= 0)
    if (e1.y > 0.0f) {
        e1 = -e1;
    }
    if (e2.y > 0.0f) {
        e2 = -e2;
    }

    // Seleccionar la arista mas cercana al eje vertical (|y| >= |x|)
    const cv::Point2f vert_edge = (std::abs(e1.y) >= std::abs(e2.y)) ? e1 : e2;

    // Calcular angulo con respecto al eje vertical superior (0, -1):
    // 0.0 deg  -> Cubo perfectamente vertical / erguido
    // > 0.0 deg -> Inclinado a la derecha (sentido horario)
    // < 0.0 deg -> Inclinado a la izquierda (sentido antihorario)
    float angle_deg = std::atan2(vert_edge.x, -vert_edge.y) * static_cast<float>(180.0 / CV_PI);

    // Normalizar al rango [-45.0 deg, +45.0 deg] aprovechando la simetria cuadratica del cubo
    while (angle_deg > 45.0f) {
        angle_deg -= 90.0f;
    }
    while (angle_deg < -45.0f) {
        angle_deg += 90.0f;
    }

    return angle_deg;
}

std::vector<CubeDetection> ColorDetector::detectSingleColor(const cv::Mat& hsv_frame,
                                                            const ColorProfile& profile) const {
    cv::Mat combined_mask;
    for (size_t i = 0; i < profile.ranges.size(); ++i) {
        cv::Mat range_mask;
        cv::inRange(hsv_frame, profile.ranges[i].lower, profile.ranges[i].upper, range_mask);
        if (i == 0) {
            combined_mask = range_mask;
        } else {
            cv::bitwise_or(combined_mask, range_mask, combined_mask);
        }
    }

    if (combined_mask.empty()) {
        return {};
    }

    // Limpieza morfológica para eliminar ruido puntual y cerrar huecos dentro del cubo
    cv::morphologyEx(combined_mask, combined_mask, cv::MORPH_OPEN, morph_kernel_);
    cv::morphologyEx(combined_mask, combined_mask, cv::MORPH_CLOSE, morph_kernel_);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(combined_mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    std::vector<CubeDetection> detections;
    for (const auto& contour : contours) {
        const double area = cv::contourArea(contour);
        if (area < config_.min_cube_area) {
            continue;
        }

        const cv::RotatedRect rrect = cv::minAreaRect(contour);
        const float w = rrect.size.width;
        const float h = rrect.size.height;
        if (w <= 1.0f || h <= 1.0f) {
            continue;
        }

        // Filtro geométrico: relación de aspecto compatible con la proyección de un cubo
        const float aspect_ratio = std::max(w, h) / std::min(w, h);
        if (aspect_ratio > config_.max_aspect_ratio) {
            continue;
        }

        // Filtro de compacidad/rectangularidad: evita manchas irregulares
        const double rect_area = static_cast<double>(w) * static_cast<double>(h);
        const double extent = area / rect_area;
        if (extent < 0.55) {
            continue;
        }

        CubeDetection cube;
        cube.color = profile.name;
        cube.x = rrect.center.x;
        cube.y = rrect.center.y;
        cube.width = w;
        cube.height = h;
        cube.area = area;
        rrect.points(cube.vertices);
        cube.angle = computeStableCubeAngleDeg(cube.vertices);

        detections.push_back(cube);
    }

    return detections;
}

void ColorDetector::drawDetectionsOverlay(cv::Mat& frame,
                                          const std::vector<CubeDetection>& cubes,
                                          double fps) const {
    for (const auto& cube : cubes) {
        cv::Scalar box_color(0, 255, 0);
        for (const auto& p : color_profiles_) {
            if (p.name == cube.color) {
                box_color = p.display_bgr;
                break;
            }
        }

        // Dibujar las 4 aristas del rectángulo rotado (minAreaRect)
        for (int i = 0; i < 4; ++i) {
            cv::line(frame, cube.vertices[i], cube.vertices[(i + 1) % 4], box_color, 2, cv::LINE_AA);
        }

        // Dibujar centro (X, Y) y linea indicadora del angulo de inclinacion respecto a la vertical
        const cv::Point2f center(cube.x, cube.y);
        cv::circle(frame, center, 5, cv::Scalar(255, 255, 255), -1, cv::LINE_AA);

        const float axis_len = std::min(cube.width, cube.height) * 0.45f;

        // Linea tenue que muestra la referencia vertical absoluta (0 deg hacia arriba)
        cv::line(frame, center, cv::Point2f(center.x, center.y - axis_len), cv::Scalar(160, 160, 160), 1, cv::LINE_AA);

        // Flecha orientada segun el angulo medido respecto a la vertical
        const float rad = cube.angle * static_cast<float>(CV_PI / 180.0);
        const cv::Point2f axis_end(
            center.x + axis_len * std::sin(rad),
            center.y - axis_len * std::cos(rad)
        );
        cv::arrowedLine(frame, center, axis_end, cv::Scalar(255, 255, 255), 2, cv::LINE_AA, 0, 0.25);

        // Etiqueta con Color, (X, Y) y Ángulo
        std::ostringstream label;
        label << cube.color << " (" << static_cast<int>( std::round(cube.x) ) << ","
              << static_cast<int>( std::round(cube.y) ) << ") "
              << std::fixed << std::setprecision(1) << cube.angle << " deg";

        const cv::Point text_pos(
            std::max(5, static_cast<int>(cube.x - cube.width * 0.45f)),
            std::max(20, static_cast<int>(cube.y - cube.height * 0.55f - 8.0f))
        );
        cv::putText(frame, label.str(), text_pos, cv::FONT_HERSHEY_SIMPLEX, 0.52,
                    cv::Scalar(0, 0, 0), 3, cv::LINE_AA);
        cv::putText(frame, label.str(), text_pos, cv::FONT_HERSHEY_SIMPLEX, 0.52,
                    box_color, 1, cv::LINE_AA);
    }

    // Banner de estado superior
    std::ostringstream hud;
    hud << "FPS: " << std::fixed << std::setprecision(1) << fps
        << " | Cubos: " << cubes.size();
    cv::putText(frame, hud.str(), cv::Point(12, 26), cv::FONT_HERSHEY_SIMPLEX, 0.65,
                cv::Scalar(0, 0, 0), 3, cv::LINE_AA);
    cv::putText(frame, hud.str(), cv::Point(12, 26), cv::FONT_HERSHEY_SIMPLEX, 0.65,
                cv::Scalar(0, 255, 180), 2, cv::LINE_AA);
}

void ColorDetector::processingLoop() {
    std::cout << "[VisionThread] Hilo de procesamiento OpenCV iniciado ("
              << color_profiles_.size() << " perfiles de color en paralelo)." << std::endl;

    uint64_t last_frame_id = 0;
    cv::Mat bgr_frame;
    cv::Mat blurred_frame;
    cv::Mat hsv_frame;

    double smoothed_fps = static_cast<double>(config_.target_fps);
    auto last_tick = std::chrono::steady_clock::now();

    const std::vector<int> jpeg_params = {cv::IMWRITE_JPEG_QUALITY, 75};

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        if (!frame_buffer_.waitForNextFrame(last_frame_id, bgr_frame, stop_flag_)) {
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - last_tick).count();
        last_tick = now;
        if (dt > 0.0001) {
            const double instant_fps = 1.0 / dt;
            smoothed_fps = 0.85 * smoothed_fps + 0.15 * instant_fps;
        }

        // Preprocesamiento: Suavizado Gaussiano + conversión a espacio HSV
        cv::GaussianBlur(bgr_frame, blurred_frame, cv::Size(5, 5), 0);
        cv::cvtColor(blurred_frame, hsv_frame, cv::COLOR_BGR2HSV);

        // Lanzar detección de cada color en paralelo (aprovecha los 4 núcleos ARM de la Raspberry Pi)
        std::vector<std::future<std::vector<CubeDetection>>> futures;
        futures.reserve(color_profiles_.size());

        for (const auto& profile : color_profiles_) {
            futures.push_back(std::async(std::launch::async, [this, &hsv_frame, &profile]() {
                return detectSingleColor(hsv_frame, profile);
            }));
        }

        std::vector<CubeDetection> all_cubes;
        for (auto& fut : futures) {
            auto color_cubes = fut.get();
            all_cubes.insert(all_cubes.end(), color_cubes.begin(), color_cubes.end());
        }

        // Construir snapshot con marca de tiempo en milisegundos
        const auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();

        DetectionSnapshot snapshot;
        snapshot.frame_id = last_frame_id;
        snapshot.timestamp_ms = epoch_ms;
        snapshot.fps = smoothed_fps;
        snapshot.frame_width = bgr_frame.cols;
        snapshot.frame_height = bgr_frame.rows;
        snapshot.cubes = std::move(all_cubes);

        // Si hay clientes viendo el stream MJPEG o la GUI local está activa, dibujamos y comprimimos JPEG
        std::vector<uchar> jpeg_buf;
        if (config_.show_gui || shared_state_.hasStreamClients()) {
            drawDetectionsOverlay(bgr_frame, snapshot.cubes, smoothed_fps);
            if (shared_state_.hasStreamClients()) {
                cv::imencode(".jpg", bgr_frame, jpeg_buf, jpeg_params);
            }
            if (config_.show_gui) {
                cv::imshow("CubeVisionTracker - OpenCV", bgr_frame);
                const int key = cv::waitKey(1) & 0xFF;
                if (key == 27 || key == 'q') {
                    stop_flag_.store(true, std::memory_order_relaxed);
                    frame_buffer_.notifyStop();
                    shared_state_.notifyStop();
                    break;
                }
            }
        }

        shared_state_.updateDetections(snapshot, jpeg_buf);
    }

    if (config_.show_gui) {
        cv::destroyAllWindows();
    }
    std::cout << "[VisionThread] Hilo de procesamiento OpenCV finalizado." << std::endl;
}

} // namespace cubetracker
