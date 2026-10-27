// Manual smoke test against real hardware: lists devices, opens the default
// duplex device for one second and prints what the engine reports.
#include <chrono>
#include <cstdio>
#include <thread>

#include "../src/looper_api.h"

int main() {
  if (looper_init() != 0) { std::printf("init failed: %s\n", looper_last_error()); return 1; }
  char* devices = looper_list_devices();
  std::printf("devices: %s\n", devices);
  looper_free_string(devices);
  if (looper_open(-1, -1, 0, 128) != 0) { std::printf("open failed: %s\n", looper_last_error()); return 1; }
  looper_play();
  std::this_thread::sleep_for(std::chrono::seconds(1));
  LooperStatus s;
  looper_get_status(&s);
  std::printf("running=%d playing=%d rate=%d inputs=%d outputs=%d blocks=%lld position=%lld reported_latency=%d frames\n",
              s.device_running, s.playing, s.sample_rate, s.input_channels, s.output_channels,
              (long long)s.block_counter, (long long)s.position, looper_reported_latency_frames());
  looper_shutdown();
  return 0;
}
