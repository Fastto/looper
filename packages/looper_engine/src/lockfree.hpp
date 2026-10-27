// Lock-free primitives used between the UI thread and the audio thread.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace looper {

// Single-producer / single-consumer ring buffer. Capacity must be a power of 2.
template <typename T, size_t Capacity>
class SpscQueue {
  static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");

 public:
  bool push(const T& item) {
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t tail = tail_.load(std::memory_order_acquire);
    if (head - tail >= Capacity) return false;
    items_[head & (Capacity - 1)] = item;
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  bool pop(T& item) {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    const size_t head = head_.load(std::memory_order_acquire);
    if (tail == head) return false;
    item = items_[tail & (Capacity - 1)];
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

 private:
  std::array<T, Capacity> items_{};
  alignas(64) std::atomic<size_t> head_{0};
  alignas(64) std::atomic<size_t> tail_{0};
};

// Triple buffer: the writer always publishes the latest complete value and
// never blocks; the reader always gets the latest complete value.
template <typename T>
class TripleBuffer {
 public:
  T& back() { return buffers_[back_]; }

  void publish() {
    back_ = middle_.exchange(back_ | kFresh, std::memory_order_acq_rel) & kIndexMask;
  }

  const T& read() {
    if (middle_.load(std::memory_order_relaxed) & kFresh) {
      front_ = middle_.exchange(front_, std::memory_order_acq_rel) & kIndexMask;
    }
    return buffers_[front_];
  }

 private:
  static constexpr uint8_t kFresh = 0x4;
  static constexpr uint8_t kIndexMask = 0x3;
  std::array<T, 3> buffers_{};
  uint8_t back_ = 0;
  uint8_t front_ = 2;
  std::atomic<uint8_t> middle_{1};
};

}  // namespace looper
