#include <cstdint>
#include <cstring>
#include <cstdio>
#include <unistd.h>

#include <vector>

#include "cxloom/common/config.h"
#include "cxloom/loommem/runtime.h"

int main() {
    char path[] = "/tmp/cxloom-region-XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0)
        return 1;
    close(fd);

    constexpr std::size_t kRegionBytes = 192ULL << 20;
    cxloom::CxloomConfig owner_config;
    owner_config.local_host_id = 0;
    owner_config.host_count = 2;
    owner_config.shared_region_bytes = kRegionBytes;
    owner_config.shared_region_path = path;
    owner_config.bootstrap_owner = true;
    owner_config.create_region_file = true;

    cxloom::loommem::LoomMemRuntime owner(owner_config);
    if (!owner.Initialize().ok()) {
        std::remove(path);
        return 1;
    }

    auto attach_config = owner_config;
    attach_config.local_host_id = 1;
    attach_config.bootstrap_owner = false;
    attach_config.create_region_file = false;

    cxloom::loommem::LoomMemRuntime attacher(attach_config);
    if (!attacher.Initialize().ok()) {
        owner.Finalize();
        std::remove(path);
        return 1;
    }

    const auto owner_object = owner.AllocateShared(sizeof(std::uint64_t), alignof(std::uint64_t));
    const auto attacher_object = attacher.AllocateShared(sizeof(std::uint64_t), alignof(std::uint64_t));
    if (!owner_object.ok() || !attacher_object.ok() || owner_object.value().offset == attacher_object.value().offset) {
        attacher.Finalize();
        owner.Finalize();
        std::remove(path);
        return 1;
    }

    const auto owner_description = attacher.DescribeSharedAllocation(owner_object.value());
    const auto attacher_description = owner.DescribeSharedAllocation(attacher_object.value());
    const auto owner_host = attacher.ResolveOwningHost(owner_object.value());
    const cxloom::GlobalPointer interior {0, owner_object.value().offset + 1};
    if (!owner_description.ok() || owner_description.value().owner_host != 0 ||
        owner_description.value().bytes != sizeof(std::uint64_t) || !attacher_description.ok() ||
        attacher_description.value().owner_host != 1 || !owner_host.ok() || owner_host.value() != 0 ||
        owner.DescribeSharedAllocation(interior).ok() || owner.ResolveLocal(interior).ok()) {
        attacher.Finalize();
        owner.Finalize();
        std::remove(path);
        return 1;
    }

    std::vector<cxloom::GlobalPointer> additional;
    for (std::size_t index = 0; index < 256; ++index) {
        const std::size_t alignment = 1ULL << (3 + index % 7);
        const auto value = owner.AllocateShared(17 + index, alignment);
        if (!value.ok() || value.value().offset % alignment != 0) {
            attacher.Finalize();
            owner.Finalize();
            std::remove(path);
            return 1;
        }
        const auto description = attacher.DescribeSharedAllocation(value.value());
        if (!description.ok() || description.value().owner_host != 0 || description.value().bytes != 17 + index ||
            description.value().alignment != alignment) {
            attacher.Finalize();
            owner.Finalize();
            std::remove(path);
            return 1;
        }
        additional.push_back(value.value());
    }

    const auto owner_local = owner.ResolveLocal(owner_object.value());
    const auto owner_from_attacher = attacher.ResolveLocal(owner_object.value());
    const auto attacher_local = attacher.ResolveLocal(attacher_object.value());
    const auto attacher_from_owner = owner.ResolveLocal(attacher_object.value());
    if (!owner_local.ok() || !owner_from_attacher.ok() || !attacher_local.ok() || !attacher_from_owner.ok()) {
        attacher.Finalize();
        owner.Finalize();
        std::remove(path);
        return 1;
    }

    constexpr std::uint64_t kOwnerValue = 0x43584c4f4f4d0000ULL;
    constexpr std::uint64_t kAttacherValue = 0x43584c4f4f4d0001ULL;
    *static_cast<std::uint64_t*>(owner_local.value()) = kOwnerValue;
    *static_cast<std::uint64_t*>(attacher_local.value()) = kAttacherValue;
    const bool values_visible = *static_cast<std::uint64_t*>(owner_from_attacher.value()) == kOwnerValue &&
                                *static_cast<std::uint64_t*>(attacher_from_owner.value()) == kAttacherValue;

    const auto owner_to_attacher = owner.GetQueue(0, 1);
    const auto owner_to_attacher_remote = attacher.GetQueue(0, 1);
    const auto attacher_to_owner = attacher.GetQueue(1, 0);
    const auto attacher_to_owner_remote = owner.GetQueue(1, 0);
    if (!owner_to_attacher.ok() || !owner_to_attacher_remote.ok() || !attacher_to_owner.ok() ||
        !attacher_to_owner_remote.ok() || owner.GetQueue(0, 0).ok()) {
        attacher.Finalize();
        owner.Finalize();
        std::remove(path);
        return 1;
    }

    auto make_message = [](cxloom::HostId source, cxloom::HostId destination, std::uint64_t value) {
        cxloom::loommem::QueueEnvelope message;
        message.header.kind = cxloom::MessageKind::kLoadUpdate;
        message.header.src_host = source;
        message.header.dst_host = destination;
        message.header.payload_bytes = sizeof(value);
        message.payload.resize(sizeof(value));
        std::memcpy(message.payload.data(), &value, sizeof(value));
        return message;
    };
    auto read_value = [](const cxloom::loommem::QueueEnvelope& message) {
        std::uint64_t value = 0;
        std::memcpy(&value, message.payload.data(), sizeof(value));
        return value;
    };

    const bool endpoint_permissions = !owner_to_attacher.value()->Pop().ok() &&
                                      !owner_to_attacher_remote.value()->Push(make_message(0, 1, 0)).ok();
    auto status = owner_to_attacher.value()->Push(make_message(0, 1, kOwnerValue));
    const auto received_by_attacher = owner_to_attacher_remote.value()->Pop();
    if (status.ok())
        status = attacher_to_owner.value()->Push(make_message(1, 0, kAttacherValue));
    const auto received_by_owner = attacher_to_owner_remote.value()->Pop();
    const bool queues_visible = endpoint_permissions && status.ok() && received_by_attacher.ok() &&
                                received_by_owner.ok() && read_value(received_by_attacher.value()) == kOwnerValue &&
                                read_value(received_by_owner.value()) == kAttacherValue;

    const bool shared_free_is_deferred = !owner.FreeShared(owner_object.value()).ok();

    attacher.Finalize();
    owner.Finalize();
    std::remove(path);
    return values_visible && queues_visible && shared_free_is_deferred ? 0 : 1;
}
