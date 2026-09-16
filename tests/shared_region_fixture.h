#pragma once

#include <cstdlib>
#include <stdexcept>
#include <unistd.h>

#include "cxloom/common/config.h"

// Exercises the production shared mapping and allocator without requiring CXL hardware.
class SharedRegionFixture {
  public:
    explicit SharedRegionFixture(cxloom::CxloomConfig& config) {
        const int fd = mkstemp(path_);
        if (fd < 0)
            throw std::runtime_error("cannot create shared test region");
        close(fd);
        config.shared_region_path = path_;
        config.bootstrap_owner = true;
        config.create_region_file = true;
    }
    ~SharedRegionFixture() { unlink(path_); }
    SharedRegionFixture(const SharedRegionFixture&) = delete;
    SharedRegionFixture& operator=(const SharedRegionFixture&) = delete;

  private:
    char path_[40] = "/tmp/cxloom-shared-test-XXXXXX";
};
