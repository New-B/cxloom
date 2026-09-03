#pragma once

#include <cstddef>
#include <string>

#include "cxloom/common/config.h"
#include "cxloom/common/status.h"

namespace cxloom::loommem {

// Maps either a shared devdax/regular-file region or a private anonymous test region.
class RegionMapper {
public:
    RegionMapper() = default;
    ~RegionMapper();

    RegionMapper(const RegionMapper&) = delete;
    RegionMapper& operator=(const RegionMapper&) = delete;

    Status Map(const CxloomConfig& config);
    Status Unmap();

    void* base() const { return base_; }
    std::size_t bytes() const { return bytes_; }
    bool is_shared() const { return shared_; }
    const std::string& path() const { return path_; }

private:
    void* base_ {nullptr};
    std::size_t bytes_ {0};
    int fd_ {-1};
    bool shared_ {false};
    std::string path_;
};

}  // namespace cxloom::loommem
