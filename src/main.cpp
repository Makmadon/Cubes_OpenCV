#include "CameraWorker.hpp"
#include "ColorDetector.hpp"
#include "HttpApiServer.hpp"
#include "ThreadSafeFrame.hpp"
#include "Types.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_stop_flag{false};

void signalHandler(int /*signum*/) {
    g_stop_flag.store(true, std::memory_order_relaxed);
}

void printUsage(const char* prog_name) {
    std::cout << "Uso: " << prog_name << " [opciones]\n\n"
              << "Opciones:\n"
              << "  --camera <id>       Indice de camara V4L2/OpenCV (por defecto: 0)\n"
              << "  --source <str>      URL de camara IP (RTSP/HTTP), ruta (/dev/video0) o pipeline GStreamer\n"
              << "  --port <puerto>     Puerto del servidor Web API HTTP (por defecto: 8080)\n"
              << "  --width <px>        Ancho de captura en pixeles (por defecto: 640)\n"
              << "  --height <px>       Alto de captura en pixeles (por defecto: 480)\n"
              << "  --fps <num>         FPS objetivo de captura (por defecto: 30)\n"
              << "  --min-area <px2>    Area minima en pixeles para considerar un cubo (por defecto: 800)\n"
              << "  --gui               Mostrar ventana local de OpenCV (imshow)\n"
              << "  --demo              Modo simulacion (genera cubos rotando sin requerir camara fisica)\n"
              << "  --help              Muestra esta ayuda\n";
}

bool parseArgs(int argc, char* argv[], cubetracker::AppConfig& config) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return false;
        } else if (arg == "--camera" && i + 1 < argc) {
            config.camera_index = std::atoi(argv[++i]);
        } else if (arg == "--source" && i + 1 < argc) {
            config.video_source = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            config.http_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--width" && i + 1 < argc) {
            config.frame_width = std::atoi(argv[++i]);
        } else if (arg == "--height" && i + 1 < argc) {
            config.frame_height = std::atoi(argv[++i]);
        } else if (arg == "--fps" && i + 1 < argc) {
            config.target_fps = std::atoi(argv[++i]);
        } else if (arg == "--min-area" && i + 1 < argc) {
            config.min_cube_area = std::atof(argv[++i]);
        } else if (arg == "--gui") {
            config.show_gui = true;
        } else if (arg == "--demo") {
            config.demo_mode = true;
        } else {
            std::cerr << "Argumento desconocido: " << arg << "\n";
            printUsage(argv[0]);
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    cubetracker::AppConfig config;
    if (!parseArgs(argc, argv, config)) {
        return 0;
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    std::cout << "=========================================================\n"
              << "  CubeVisionTracker C++17 (OpenCV + Hilos + Web API)\n"
              << "=========================================================\n";

    cubetracker::LatestFrameBuffer frame_buffer;
    cubetracker::SharedDetectionState shared_state;

    // 1. Iniciar hilo de captura de cámara (Producer)
    cubetracker::CameraWorker camera_worker(config, frame_buffer, g_stop_flag);
    // 2. Iniciar hilo de visión artificial y detección paralela por color (Worker)
    cubetracker::ColorDetector color_detector(config, frame_buffer, shared_state, g_stop_flag);
    // 3. Iniciar hilo del servidor Web API (Consumer HTTP / JSON / MJPEG)
    cubetracker::HttpApiServer api_server(config.http_port, shared_state, g_stop_flag);

    camera_worker.start();
    color_detector.start();
    api_server.start();

    std::cout << "[Main] Sistema en ejecucion. Presiona Ctrl+C para detener.\n";

    while (!g_stop_flag.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

    std::cout << "\n[Main] Deteniendo hilos de forma segura...\n";
    frame_buffer.notifyStop();
    shared_state.notifyStop();

    camera_worker.join();
    color_detector.join();
    api_server.join();

    std::cout << "[Main] Programa finalizado correctamente.\n";
    return 0;
}
