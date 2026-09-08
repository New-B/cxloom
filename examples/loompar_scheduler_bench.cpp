// Deterministic placement simulation using the production scheduler.
// Times are model units, never measured CXL latency or application throughput.
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
#include "cxloom/loompar/scheduler.h"

int main() {
    using namespace cxloom;
    constexpr unsigned hosts = 16, tasks = 4096;
    std::cout << "scenario,policy,tasks,mean_latency_units,p95_latency_units,makespan_units,max_host_tasks\n";
    for (const std::string scenario : {"uniform", "skewed", "queue_pressure"}) {
        for (const std::string policy : {"round_robin", "least_loaded", "weighted"}) {
            CxloomConfig config; config.host_count = hosts;
            config.scheduler_history_weight = 0;
            config.scheduler_queue_weight = 1;
            loompar::PlacementScheduler scheduler(config, nullptr);
            std::array<double, hosts> available{};
            std::array<unsigned, hosts> counts{};
            std::array<std::vector<double>, hosts> completions;
            std::vector<double> latency;
            for (unsigned task = 0; task < tasks; ++task) {
                const double arrival = task * 0.25;
                std::vector<HostLoadSnapshot> samples;
                for (unsigned h = 0; h < hosts; ++h) {
                    auto& pending = completions[h];
                    pending.erase(std::remove_if(pending.begin(), pending.end(),
                        [&](double finish) { return finish <= arrival; }), pending.end());
                    // Persistent transport backlog on four hosts in this scenario.
                    const unsigned queue = scenario == "queue_pressure" && h < 4 ? 8 : 0;
                    samples.push_back({static_cast<HostId>(h), static_cast<unsigned>(pending.size()), 0, queue});
                }
                HostId selected = task % hosts;
                if (policy == "least_loaded") {
                    selected = std::min_element(samples.begin(), samples.end(),
                        [](const auto& a, const auto& b) { return a.running_threads < b.running_threads; })->host_id;
                } else if (policy == "weighted") {
                    auto result = scheduler.SelectHost({}, samples);
                    if (!result.ok()) { std::cerr << result.status().message() << '\n'; return 1; }
                    selected = result.value();
                }
                const double service = scenario == "skewed" && task % 7 == 0 ? 40 : 2;
                const double finish = std::max(arrival + samples[selected].queued_messages,
                                               available[selected]) + service;
                available[selected] = finish;
                completions[selected].push_back(finish);
                ++counts[selected];
                latency.push_back(finish - arrival);
            }
            double total = 0;
            for (double value : latency) total += value;
            std::sort(latency.begin(), latency.end());
            std::cout << scenario << ',' << policy << ',' << tasks << ',' << total / tasks << ','
                      << latency[static_cast<std::size_t>(tasks * 0.95)] << ','
                      << *std::max_element(available.begin(), available.end()) << ','
                      << *std::max_element(counts.begin(), counts.end()) << '\n';
        }
    }
}
