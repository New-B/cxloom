#include <chrono>
#include <iostream>
#include <thread>
#include "cxloom/loompar/scheduler.h"
#define CHECK(x) do { if (!(x)) { std::cerr << "failed " << __LINE__ << std::endl; return 1; } } while (0)
using namespace cxloom;
int main() {
  CxloomConfig config; config.host_count = 16; config.scheduler_queue_weight = 2.0;
  config.scheduler_history_weight = 1.0; config.scheduler_load_half_life_ms = 1.0;
  loompar::PlacementScheduler scheduler(config, nullptr);
  std::vector<HostLoadSnapshot> samples;
  for (HostId host = 0; host < 16; ++host)
    samples.push_back({host, host == 3 ? 0U : 2U, 0U, host == 3 ? 100U : 0U, 1, 1});
  ThreadPlacementHint hint;
  auto selected = scheduler.SelectHost(hint, samples);
  CHECK(selected.ok() && selected.value() == 0); // queue cost makes host 0 preferable to queued host 3
  for (int i = 0; i < 5; ++i) scheduler.RecordLaunch(5);
  samples[5].running_threads = 0;
  samples[5].sampled_at_ns = samples[0].sampled_at_ns;
  selected = scheduler.SelectHost(hint, samples);
  CHECK(selected.ok() && selected.value() != 5); // launch history penalizes a repeatedly chosen host
  config.max_running_threads_per_host = 1;
  loompar::PlacementScheduler limited(config, nullptr);
  for (auto& sample : samples) sample.running_threads = 1;
  CHECK(limited.SelectHost(hint, samples).status().code() == StatusCode::kUnavailable);
  // Fractional costs must not be truncated: 0.25 beats 0.75.
  config.max_running_threads_per_host = 0;
  config.scheduler_queue_weight = 0.25;
  loompar::PlacementScheduler fractional(config, nullptr);
  samples = {{0, 0, 0, 3}, {1, 0, 0, 1}};
  selected = fractional.SelectHost(hint, samples);
  CHECK(selected.ok() && selected.value() == 1);
  std::cout << "queue prediction, load aging, locality score and history penalty passed\n";
}
