#pragma once

#include "Types.hpp"
#include <opencv2/core.hpp>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace cubetracker {

/**
 * @brief Buffer concurrente de ranura única (Single-Slot Overwrite Buffer).
 *        Garantiza que el hilo de visión siempre procese el frame más reciente
 *        de la cámara sin acumular latencia (lag) en la Raspberry Pi.
 */
class LatestFrameBuffer {
public:
    void publish(const cv::Mat& frame) {
        if (frame.empty()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            frame.copyTo(latest_frame_);
            ++frame_id_;
        }
        cv_.notify_all();
    }

    /**
     * @brief Espera hasta que exista un frame más nuevo que last_frame_id o se detenga el sistema.
     * @return true si se obtuvo un nuevo frame, false si se solicitó parada.
     */
    bool waitForNextFrame(uint64_t& last_frame_id, cv::Mat& out_frame, const std::atomic<bool>& stop_flag) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&]() {
            return stop_flag.load(std::memory_order_relaxed) || (frame_id_ > last_frame_id && !latest_frame_.empty());
        });

        if (stop_flag.load(std::memory_order_relaxed)) {
            return false;
        }

        latest_frame_.copyTo(out_frame);
        last_frame_id = frame_id_;
        return true;
    }

    void notifyStop() {
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    cv::Mat latest_frame_;
    uint64_t frame_id_ = 0;
};

/**
 * @brief Estado compartido thread-safe entre el hilo de Visión Artificial
 *        y los hilos del Servidor Web API.
 */
class SharedDetectionState {
public:
    void updateDetections(const DetectionSnapshot& snapshot, const std::vector<uchar>& jpeg_buffer) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_ = snapshot;
            if (!jpeg_buffer.empty()) {
                latest_jpeg_ = jpeg_buffer;
            }
        }
        cv_.notify_all();
    }

    DetectionSnapshot getSnapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_;
    }

    bool waitForNextJpeg(uint64_t& last_frame_id, std::vector<uchar>& out_jpeg, const std::atomic<bool>& stop_flag) const {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&]() {
            return stop_flag.load(std::memory_order_relaxed) ||
                   (snapshot_.frame_id > last_frame_id && !latest_jpeg_.empty());
        });

        if (stop_flag.load(std::memory_order_relaxed)) {
            return false;
        }

        out_jpeg = latest_jpeg_;
        last_frame_id = snapshot_.frame_id;
        return true;
    }

    void notifyStop() {
        cv_.notify_all();
    }

    void incrementStreamClients() {
        active_stream_clients_.fetch_add(1, std::memory_order_relaxed);
    }

    void decrementStreamClients() {
        active_stream_clients_.fetch_sub(1, std::memory_order_relaxed);
    }

    bool hasStreamClients() const {
        return active_stream_clients_.load(std::memory_order_relaxed) > 0;
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    DetectionSnapshot snapshot_;
    std::vector<uchar> latest_jpeg_;
    std::atomic<int> active_stream_clients_{0};
};

} // namespace cubetracker
