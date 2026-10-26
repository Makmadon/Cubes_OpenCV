# CubeVisionTracker (C++17 + OpenCV + Hilos + Web API)

Sistema de visión artificial de alto rendimiento escrito en **C++17** con **OpenCV nativo** y arquitectura **multihilo**, diseñado para ejecutarse con baja latencia dentro de una **Raspberry Pi**.

Reconoce cubos de colores (`Rojo`, `Verde`, `Azul`, `Amarillo`), calcula su centro `(X, Y)` y su **ángulo de inclinación 2D** en tiempo real mediante cajas delimitadoras rotadas (`cv::minAreaRect`), y expone los resultados a través de un **Servidor Web API HTTP** integrado (sin dependencias externas).

---

## Arquitectura de Hilos

1. **Hilo de Captura (`CameraWorker`)**:
   - Captura frames continuamente desde la cámara (`V4L2` en Linux/Raspberry Pi) con buffer de tamaño `1` (`cv::CAP_PROP_BUFFERSIZE = 1`) para eliminar la acumulación de *lag*.
   - Publica siempre el último frame en un buffer concurrente sin bloqueo (`LatestFrameBuffer`).
2. **Hilo de Visión + Pool Paralelo por Color (`ColorDetector`)**:
   - Toma el frame más reciente, aplica filtro Gaussiano y transforma a espacio de color **HSV**.
   - Lanza tareas concurrentes (`std::async`) por cada perfil de color para aprovechar todos los núcleos ARM de la Raspberry Pi simultáneamente.
   - Aplica operaciones morfológicas (`MORPH_OPEN` / `MORPH_CLOSE`), filtra contornos por área/relación de aspecto/rectangularidad y obtiene la caja rotada con `cv::minAreaRect`.
   - Calcula el ángulo de inclinación 2D estabilizado en el rango `[-45.0°, +45.0°]` respecto al eje vertical (0° = vertical/erguido, > 0° = inclinado a la derecha, < 0° = inclinado a la izquierda).
3. **Hilo de Servidor Web API (`HttpApiServer`)**:
   - Sirve peticiones HTTP concurrentes sin bloquear el bucle de visión artificial.
   - Solo comprime imágenes JPEG cuando hay algún cliente conectado a `/api/stream`, ahorrando CPU en modo *headless*.

---

## Instalación y Compilación en Raspberry Pi

En Raspberry Pi OS (Bookworm / Bullseye) solo necesitas instalar las herramientas base y `libopencv-dev`:

```bash
sudo apt update
sudo apt install -y build-essential cmake libopencv-dev
```

Compilar el proyecto en modo `Release` (activa `-O3`):

```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

---

## Ejecución

### 1. Con cámara física en la Raspberry Pi (Modo Headless + Web API)
```bash
./cube_tracker --camera 0 --port 8080 --width 640 --height 480 --fps 30
```

### 2. Con Cámara IP (RTSP, ESP32-CAM o HTTP)
```bash
# Cámara de seguridad IP / RTSP:
./cube_tracker --source "rtsp://admin:12345@192.168.1.50:554/stream1" --port 8080

# ESP32-CAM o App IP Webcam:
./cube_tracker --source "http://192.168.1.50:81/stream" --port 8080
```

### 3. Modo Simulación (`--demo`) para probar sin cámara conectada
```bash
./cube_tracker --demo --port 8080
```

### 4. Opciones de línea de comandos
| Argumento | Descripción | Valor por defecto |
| :--- | :--- | :--- |
| `--camera <id>` | Índice de la cámara (`/dev/video0` = `0`) | `0` |
| `--source <str>` | URL de cámara IP (RTSP/HTTP) o pipeline GStreamer | *(vacío)* |
| `--port <puerto>` | Puerto TCP del servidor Web API | `8080` |
| `--width <px>` | Ancho de captura en píxeles | `640` |
| `--height <px>` | Alto de captura en píxeles | `480` |
| `--fps <num>` | FPS objetivo | `30` |
| `--min-area <px2>` | Área mínima para detectar un cubo | `800` |
| `--gui` | Abre ventana local con `cv::imshow` | Desactivado |
| `--demo` | Genera cubos sintéticos rotando | Desactivado |

---

## Endpoints del Servidor Web API

Una vez iniciado el binario, puedes acceder desde cualquier dispositivo en la red local:

### 1. `GET /api/cubes` — Datos JSON en Tiempo Real `(X, Y, Color, Ángulo)`
```bash
curl http://localhost:8080/api/cubes
```

Respuesta JSON:
```json
{
  "frame_id": 245,
  "timestamp_ms": 1729948102450,
  "fps": 30.00,
  "resolution": { "width": 640, "height": 480 },
  "count": 2,
  "cubes": [
    {
      "color": "Rojo",
      "x": 164.50,
      "y": 172.10,
      "angle": 21.40,
      "width": 90.00,
      "height": 90.00,
      "area": 8100.00
    },
    {
      "color": "Azul",
      "x": 452.00,
      "y": 318.80,
      "angle": -14.20,
      "width": 95.00,
      "height": 95.00,
      "area": 9025.00
    }
  ]
}
```

### 2. `GET /api/stream` — Video MJPEG anotado en vivo
Transmite el video procesado con las cajas rotadas, el centro `(X, Y)` y el vector del ángulo de inclinación dibujados.

### 3. `GET /` — Panel de Control Web Integrado
Abre `http://<IP-DE-TU-RASPBERRY>:8080/` en cualquier navegador para ver tanto el video en vivo como la tabla de telemetría y el JSON en tiempo real.
