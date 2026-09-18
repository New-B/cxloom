#pragma once

#include <optional>
#include <unordered_map>
#include "cxloom/loommem/queue.h"
#include "cxloom/loompar/threading.h"

namespace cxloom::loompar {

constexpr std::size_t kMaxClusterFunctions = 256;
constexpr std::size_t kMaxClusterFunctionName = 63;
constexpr std::size_t kMaxClusterArgumentBytes = 80;

struct ClusterFunction {
    std::string name;
    std::uint32_t abi_version{0};
    std::uint32_t argument_bytes{0};
    std::uint64_t schema_id{0};
    // Local binding only. Neither this identity nor the callable crosses a queue.
    std::uintptr_t local_identity{0};
    ThreadFunction function;
};

// One immutable manifest per bootstrap session. Advertisements are emitted only
// after every local callback has been installed. Receiving every peer's matching
// manifest therefore establishes that all possible execution hosts can resolve it.
class ClusterFunctionRegistry {
public:
    ClusterFunctionRegistry(HostId local, std::uint16_t hosts) : local_(local), hosts_(hosts), peers_(hosts) {}
    Result<std::vector<loommem::QueueEnvelope>> Install(std::vector<ClusterFunction> functions);
    Status Handle(const loommem::QueueEnvelope& message);
    Status Wait(std::uint64_t timeout_ms);
    bool installed() const;
    Result<std::uint64_t> Lookup(std::uintptr_t identity) const;
    Result<ThreadFunction> Resolve(std::uint64_t id, std::size_t argument_bytes, bool require_ready) const;

private:
    struct Descriptor {
        std::string name;
        std::uint32_t abi_version{0};
        std::uint32_t argument_bytes{0};
        std::uint64_t schema_id{0};
        bool operator==(const Descriptor& other) const {
            return name == other.name && abi_version == other.abi_version &&
                   argument_bytes == other.argument_bytes && schema_id == other.schema_id;
        }
    };
    struct Peer {
        bool seen{false};
        std::vector<std::optional<Descriptor>> entries;
    };
    static std::uint64_t FunctionId(const std::string& name);
    void CheckLocked();
    HostId local_;
    std::uint16_t hosts_;
    mutable std::mutex mutex_;
    Condition changed_;
    bool installed_{false};
    bool ready_{false};
    Status failure_;
    std::vector<ClusterFunction> functions_;
    std::vector<Descriptor> descriptors_;
    std::unordered_map<std::uint64_t, std::size_t> by_id_;
    std::vector<Peer> peers_;
};
}  // namespace cxloom::loompar
