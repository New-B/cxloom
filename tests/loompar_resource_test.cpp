#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <thread>
#include "cxloom/loompar/runtime.h"
#define CHECK(x) do { if (!(x)) { std::cerr << "failed " << __LINE__ << std::endl; return 1; } } while (0)
using namespace cxloom;
std::atomic<bool> release_thread{false};
void Hold(void*) { while (!release_thread.load()) std::this_thread::yield(); }
int main() {
  CxloomConfig config; config.shared_region_bytes = 192ULL << 20; config.max_running_threads_per_host = 1;
  loommem::LoomMemRuntime mem(config); CHECK(mem.Initialize().ok());
  loompar::LoomParRuntime par(config, &mem); CHECK(par.Initialize().ok());
  CHECK(par.RegisterFunction("hold", Hold).ok());
  auto one = par.CreateThread("hold", {}, {}); CHECK(one.ok());
  auto blocked = par.CreateThread("hold", {}, {}); CHECK(!blocked.ok());
  CHECK(blocked.status().code() == StatusCode::kUnavailable);
  release_thread = true; CHECK(par.JoinThread(one.value()).ok());
  auto two = par.CreateThread("hold", {}, {}); CHECK(two.ok()); release_thread = true; CHECK(par.JoinThread(two.value()).ok());
  CHECK(par.Finalize().ok()); CHECK(mem.Finalize().ok());
  std::cout << "per-host running limit and release passed\n";
}
