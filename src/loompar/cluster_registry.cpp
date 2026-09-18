#include "cxloom/loompar/cluster_registry.h"

#include <algorithm>
#include <cstring>
#include <unordered_set>

namespace cxloom::loompar {
namespace {
constexpr std::uint32_t kMagic = 0x434c464e; // CLFN
constexpr std::uint16_t kVersion = 1;
struct ManifestWire {
    std::uint32_t magic{kMagic};
    std::uint16_t version{kVersion};
    std::uint16_t count{0};
    std::uint32_t index{0};
    std::uint32_t abi_version{0};
    std::uint64_t schema_id{0};
    std::uint32_t argument_bytes{0};
    std::uint32_t platform_abi{0};
    char name[64]{};
};
static_assert(sizeof(ManifestWire) == 96);
static_assert(sizeof(ManifestWire) <= loommem::kQueuePayloadBytes);
std::uint32_t PlatformAbi() {
    const std::uint16_t value = 1;
    const auto endian = *reinterpret_cast<const unsigned char*>(&value) == 1 ? 1U : 2U;
    return endian | (sizeof(void*) << 8) | (sizeof(std::size_t) << 16);
}
}

std::uint64_t ClusterFunctionRegistry::FunctionId(const std::string& name) {
    std::uint64_t id = 14695981039346656037ULL;
    for (unsigned char c : name) { id ^= c; id *= 1099511628211ULL; }
    return id;
}

Result<std::vector<loommem::QueueEnvelope>> ClusterFunctionRegistry::Install(std::vector<ClusterFunction> functions) {
    if (!hosts_ || local_ >= hosts_ || functions.size() > kMaxClusterFunctions)
        return Status::InvalidArgument("invalid cluster manifest size or host configuration");
    std::sort(functions.begin(), functions.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    std::vector<Descriptor> descriptors;
    std::unordered_map<std::uint64_t, std::size_t> ids;
    std::unordered_set<std::uintptr_t> identities;
    for (const auto& function : functions) {
        if (function.name.empty() || function.name.size() > kMaxClusterFunctionName ||
            function.name.find('\0') != std::string::npos || !function.function || !function.local_identity ||
            !function.abi_version || !function.schema_id || function.argument_bytes > kMaxClusterArgumentBytes)
            return Status::InvalidArgument("invalid cluster function name, binding, ABI, schema or argument size");
        if (!ids.emplace(FunctionId(function.name), descriptors.size()).second ||
            !identities.insert(function.local_identity).second)
            return Status::AlreadyExists("duplicate cluster function name/ID or callback identity");
        descriptors.push_back({function.name, function.abi_version, function.argument_bytes, function.schema_id});
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (installed_) {
        if (descriptors != descriptors_)
            return Status::FailedPrecondition("cluster manifest is immutable for this runtime");
        for (std::size_t i = 0; i < functions.size(); ++i)
            if (functions[i].local_identity != functions_[i].local_identity)
                return Status::AlreadyExists("cluster callback binding cannot change");
        // Timeout retries only wait again. Original publications remain queued;
        // replaying them would leave unnecessary traffic after the rendezvous.
        return std::vector<loommem::QueueEnvelope>{};
    }
    functions_ = std::move(functions);
    descriptors_ = std::move(descriptors);
    by_id_ = std::move(ids);
    installed_ = true;
    std::vector<loommem::QueueEnvelope> messages;
    for (HostId host = 0; host < hosts_; ++host) {
        if (host == local_) continue;
        // An empty manifest still advertises its presence.
        for (std::size_t i = 0; i < std::max<std::size_t>(1, descriptors_.size()); ++i) {
            ManifestWire wire{};
            wire.platform_abi = PlatformAbi();
            wire.count = static_cast<std::uint16_t>(descriptors_.size());
            wire.index = static_cast<std::uint32_t>(i);
            if (!descriptors_.empty()) {
                const auto& descriptor = descriptors_[i];
                wire.abi_version = descriptor.abi_version;
                wire.argument_bytes = descriptor.argument_bytes;
                wire.schema_id = descriptor.schema_id;
                std::memcpy(wire.name, descriptor.name.data(), descriptor.name.size());
            }
            loommem::QueueEnvelope message;
            message.header = {MessageKind::kFunctionManifest, local_, host, sizeof(wire)};
            message.payload.resize(sizeof(wire));
            std::memcpy(message.payload.data(), &wire, sizeof(wire));
            messages.push_back(std::move(message));
        }
    }
    CheckLocked();
    return messages;
}

void ClusterFunctionRegistry::CheckLocked() {
    if (!installed_ || !failure_.ok()) { changed_.notify_all(); return; }
    bool complete = true;
    for (HostId host = 0; host < hosts_; ++host) {
        if (host == local_) continue;
        const auto& peer = peers_[host];
        if (!peer.seen) { complete = false; continue; }
        if (peer.entries.size() != descriptors_.size()) {
            failure_ = Status::FailedPrecondition("cluster function count mismatch on host " + std::to_string(host));
            break;
        }
        for (std::size_t i = 0; i < descriptors_.size(); ++i) {
            if (!peer.entries[i]) { complete = false; continue; }
            if (!(*peer.entries[i] == descriptors_[i])) {
                failure_ = Status::FailedPrecondition("cluster function ABI/schema/name mismatch on host " + std::to_string(host));
                break;
            }
        }
    }
    ready_ = failure_.ok() && complete;
    changed_.notify_all();
}

Status ClusterFunctionRegistry::Handle(const loommem::QueueEnvelope& message) {
    if (message.header.kind != MessageKind::kFunctionManifest || message.header.dst_host != local_ ||
        message.header.src_host >= hosts_ || message.header.src_host == local_)
        return Status::InvalidArgument("invalid manifest endpoint");
    std::lock_guard<std::mutex> lock(mutex_);
    auto reject = [&] {
        ready_ = false;
        failure_ = Status::FailedPrecondition("invalid or conflicting peer function manifest");
        changed_.notify_all();
        return Status::InvalidArgument("invalid or conflicting peer function manifest");
    };
    if (message.payload.size() != sizeof(ManifestWire) || message.header.payload_bytes != sizeof(ManifestWire)) return reject();
    ManifestWire wire{};
    std::memcpy(&wire, message.payload.data(), sizeof(wire));
    if (wire.magic != kMagic || wire.version != kVersion || wire.platform_abi != PlatformAbi() ||
        wire.count > kMaxClusterFunctions || wire.index >= std::max<unsigned>(1, wire.count) ||
        !std::memchr(wire.name, 0, sizeof(wire.name))) return reject();
    if (wire.count && (!wire.name[0] || !wire.abi_version || !wire.schema_id || wire.argument_bytes > kMaxClusterArgumentBytes))
        return reject();
    auto& peer = peers_[message.header.src_host];
    if (peer.seen && peer.entries.size() != wire.count) return reject();
    if (!peer.seen) { peer.seen = true; peer.entries.resize(wire.count); }
    if (wire.count) {
        Descriptor descriptor{wire.name, wire.abi_version, wire.argument_bytes, wire.schema_id};
        auto& slot = peer.entries[wire.index];
        if (slot && !(*slot == descriptor)) return reject();
        slot = std::move(descriptor);
    }
    CheckLocked();
    return Status::Ok();
}

Status ClusterFunctionRegistry::Wait(std::uint64_t timeout_ms) {
    if (!timeout_ms || timeout_ms > UINT32_MAX) return Status::InvalidArgument("invalid manifest timeout");
    std::unique_lock<std::mutex> lock(mutex_);
    if (!installed_) return Status::FailedPrecondition("register the cluster function manifest before create");
    if (!changed_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return ready_ || !failure_.ok(); }))
        return Status::Unavailable("timed out waiting for all host function manifests; retry the same manifest");
    return failure_;
}

bool ClusterFunctionRegistry::installed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return installed_;
}

Result<std::uint64_t> ClusterFunctionRegistry::Lookup(std::uintptr_t identity) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!installed_) return Status::FailedPrecondition("register the cluster function manifest before create");
    for (const auto& function : functions_)
        if (function.local_identity == identity) return FunctionId(function.name);
    return Status::NotFound("callback is absent from the cluster manifest");
}

Result<ThreadFunction> ClusterFunctionRegistry::Resolve(std::uint64_t id, std::size_t argument_bytes, bool require_ready) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!failure_.ok()) return failure_;
    if (!installed_ || (require_ready && !ready_)) return Status::FailedPrecondition("cluster function manifest is not ready");
    auto it = by_id_.find(id);
    if (it == by_id_.end()) return Status::NotFound("unknown cluster function ID");
    const auto& function = functions_[it->second];
    if (argument_bytes != function.argument_bytes) return Status::InvalidArgument("argument size differs from cluster function schema");
    return function.function;
}
}  // namespace cxloom::loompar
