cmake_minimum_required(VERSION 3.16)
project(CubeVisionTracker VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Optimizaciones por defecto en modo Release (ideal para Raspberry Pi)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Buscar dependencias: Hilos del sistema (pthread) y OpenCV
find_package(Threads REQUIRED)
find_package(OpenCV REQUIRED)

message(STATUS "OpenCV version: ${OpenCV_VERSION}")
message(STATUS "OpenCV include dirs: ${OpenCV_INCLUDE_DIRS}")
message(STATUS "OpenCV libraries: ${OpenCV_LIBS}")

include_directories(
    ${PROJECT_SOURCE_DIR}/include
    ${OpenCV_INCLUDE_DIRS}
)

set(SOURCES
    src/main.cpp
    src/CameraWorker.cpp
    src/ColorDetector.cpp
    src/HttpApiServer.cpp
)

add_executable(cube_tracker ${SOURCES})

target_link_libraries(cube_tracker
    PRIVATE
    ${OpenCV_LIBS}
    Threads::Threads
)

# En Windows enlazar Winsock2 para el servidor HTTP; en Linux/Raspberry Pi activar flags de optimización
if(WIN32)
    target_link_libraries(cube_tracker PRIVATE ws2_32)
else()
    target_compile_options(cube_tracker PRIVATE -Wall -Wextra -O3)
endif()
