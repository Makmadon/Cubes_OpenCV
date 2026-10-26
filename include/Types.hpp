#pragma once

#include <opencv2/core.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace cubetracker {

/**
 * @brief Rango de color en espacio HSV (Hue: 0-180, Saturation: 0-255, Value: 0-255 en OpenCV).
 */
struct HsvRange {
    cv::Scalar lower;
    cv::Scalar upper;
};

/**
 * @brief Perfil de un color a detectar. Soporta múltiples rangos HSV
 *        (por ejemplo, el color Rojo cruza el límite 0/180 del canal Hue).
 */
struct ColorProfile {
    std::string name;
    cv::Scalar display_bgr;
    std::vector<HsvRange> ranges;
};

/**
 * @brief Datos de un cubo detectado en el plano 2D de la cámara.
 */
struct CubeDetection {
    std::string color;          // Nombre del color detectado
    float x = 0.0f;             // Centro X en píxeles
    float y = 0.0f;             // Centro Y en píxeles
    float width = 0.0f;         // Ancho de la caja rotada (px)
    float height = 0.0f;        // Alto de la caja rotada (px)
    float angle = 0.0f;         // Ángulo de inclinación 2D respecto a la vertical [-45°, +45°] (0° = erguido, >0 der, <0 izq)
    double area = 0.0;          // Área del contorno (px^2)
    cv::Point2f vertices[4];    // 4 esquinas del rectángulo rotado (minAreaRect)
};

/**
 * @brief Instantánea inmutable de resultados publicada hacia el servidor Web API.
 */
struct DetectionSnapshot {
    uint64_t frame_id = 0;
    int64_t timestamp_ms = 0;
    double fps = 0.0;
    int frame_width = 0;
    int frame_height = 0;
    std::vector<CubeDetection> cubes;
};

/**
 * @brief Configuración general de la aplicación.
 */
struct AppConfig {
    int camera_index = 0;
    std::string video_source = ""; // Vacío = usar camera_index; permite ruta /dev/video0 o pipeline GStreamer
    int frame_width = 640;
    int frame_height = 480;
    int target_fps = 30;
    uint16_t http_port = 8080;
    double min_cube_area = 800.0;
    double max_aspect_ratio = 2.2; // Filtra objetos muy alargados que no son cubos
    bool show_gui = false;         // false por defecto para ejecución headless en Raspberry Pi
    bool demo_mode = false;        // Genera cubos sintéticos en rotación para pruebas sin cámara física
};

} // namespace cubetracker
