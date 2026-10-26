#pragma once

#include "ThreadSafeFrame.hpp"
#include "Types.hpp"
#include <opencv2/core.hpp>
#include <atomic>
#include <thread>
#include <vector>

namespace cubetracker {

/**
 * @brief Procesador de visión artificial multihilo:
 *        - Recibe el frame más reciente desde LatestFrameBuffer.
 *        - Ejecuta segmentación HSV en paralelo por cada perfil de color.
 *        - Calcula el centro (X, Y) y el ángulo de inclinación 2D con cv::minAreaRect.
 *        - Publica los resultados en SharedDetectionState para la Web API.
 */
class ColorDetector {
public:
    ColorDetector(const AppConfig& config,
                  LatestFrameBuffer& frame_buffer,
                  SharedDetectionState& shared_state,
                  std::atomic<bool>& stop_flag);
    ~ColorDetector();

    ColorDetector(const ColorDetector&) = delete;
    ColorDetector& operator=(const ColorDetector&) = delete;

    void start();
    void join();

    static std::vector<ColorProfile> defaultColorProfiles();
    static float computeStableCubeAngleDeg(const cv::Point2f pts[4]);

private:
    void processingLoop();
    std::vector<CubeDetection> detectSingleColor(const cv::Mat& hsv_frame, const ColorProfile& profile) const;
    void drawDetectionsOverlay(cv::Mat& frame, const std::vector<CubeDetection>& cubes, double fps) const;

    AppConfig config_;
    LatestFrameBuffer& frame_buffer_;
    SharedDetectionState& shared_state_;
    std::atomic<bool>& stop_flag_;
    std::vector<ColorProfile> color_profiles_;
    cv::Mat morph_kernel_;
    std::thread worker_thread_;
};

} // namespace cubetracker
