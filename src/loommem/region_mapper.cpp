#include "cxloom/loommem/region_mapper.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace cxloom::loommem {

namespace {

Status SystemError(const std::string& action, const std::string& path) {
    return Status::Unavailable(action + " " + path + ": " + std::strerror(errno));
}

}  // namespace

RegionMapper::~RegionMapper() {
    Unmap();
}

Status RegionMapper::Map(const CxloomConfig& config) {
    if (base_ != nullptr) {
        return Status::FailedPrecondition("shared region is already mapped");
    }

    bytes_ = config.shared_region_bytes;
    path_ = config.shared_region_path;
    if (path_.empty()) {
        base_ = mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base_ == MAP_FAILED) {
            base_ = nullptr;
            return SystemError("mmap anonymous region", "");
        }
        return Status::Ok();
    }

    fd_ = open(path_.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        return SystemError("open", path_);
    }

    struct stat file_stat {};
    if (fstat(fd_, &file_stat) != 0) {
        const auto status = SystemError("fstat", path_);
        Unmap();
        return status;
    }
    if (S_ISREG(file_stat.st_mode) && static_cast<std::uint64_t>(file_stat.st_size) < bytes_) {
        if (!config.create_region_file) {
            Unmap();
            return Status::InvalidArgument("shared region file is smaller than shared_region_bytes");
        }
        if (ftruncate(fd_, static_cast<off_t>(bytes_)) != 0) {
            const auto status = SystemError("resize", path_);
            Unmap();
            return status;
        }
    }

    base_ = mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (base_ == MAP_FAILED) {
        base_ = nullptr;
        const auto status = SystemError("mmap", path_);
        Unmap();
        return status;
    }
    shared_ = true;
    return Status::Ok();
}

Status RegionMapper::Unmap() {
    Status status = Status::Ok();
    if (base_ != nullptr) {
        if (munmap(base_, bytes_) != 0) {
            status = SystemError("munmap", path_);
        }
        base_ = nullptr;
        bytes_ = 0;
    }
    if (fd_ >= 0) {
        if (close(fd_) != 0 && status.ok()) {
            status = SystemError("close", path_);
        }
        fd_ = -1;
    }
    shared_ = false;
    path_.clear();
    return status;
}

}  // namespace cxloom::loommem
