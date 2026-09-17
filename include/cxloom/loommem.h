#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "cxloom/common/config.h"
#include "cxloom/common/status.h"
#include "cxloom/common/types.h"

namespace cxloom {

using GPtr = GlobalPointer;

struct AllocOptions {
    std::size_t bytes {0};
    std::size_t alignment {0};
    // Zero uses the runtime block size.
    std::size_t coherence_block_bytes {0};
};

class Context {
  public:
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

  private:
    struct Impl;
    explicit Context(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;

    friend Result<std::unique_ptr<Context>> clInit(CxloomConfig config);
    friend Status clDestroy(std::unique_ptr<Context>& context);
    friend Result<GPtr> clAlloc(Context&, std::size_t, std::size_t);
    friend Result<GPtr> clAlloc(Context&, const AllocOptions&);
    friend Result<class ReadView> clRead(Context&, GPtr, std::uint64_t);
    friend Result<class ReadView> clReadRange(Context&, GPtr, std::uint64_t, std::uint64_t,
                                               std::uint64_t);
    friend Result<class WriteView> clWrite(Context&, GPtr, std::uint64_t);
    friend Result<class WriteView> clWriteRange(Context&, GPtr, std::uint64_t, std::uint64_t,
                                                 std::uint64_t);
    friend Status clFree(Context&, GPtr);
    friend Status clInvalidate(Context&, GPtr);
    friend Status clSynchronizeAcquire(Context&);
    friend Status clSynchronizeRelease(Context&);
    friend class WriteView;
};

class ReadView {
  public:
    ReadView() = default;
    const void* data() const;
    std::size_t size() const;
    explicit operator bool() const { return storage_ != nullptr; }

  private:
    explicit ReadView(std::shared_ptr<const void> storage, const void* data, std::size_t size);
    std::shared_ptr<const void> storage_;
    const void* data_ {nullptr};
    std::size_t size_ {0};

    friend Result<ReadView> clRead(Context&, GPtr, std::uint64_t);
    friend Result<ReadView> clReadRange(Context&, GPtr, std::uint64_t, std::uint64_t,
                                         std::uint64_t);
};

class WriteView {
  public:
    WriteView();
    ~WriteView();
    WriteView(WriteView&&) noexcept;
    WriteView& operator=(WriteView&&) noexcept;
    WriteView(const WriteView&) = delete;
    WriteView& operator=(const WriteView&) = delete;

    void* data();
    const void* data() const;
    std::size_t size() const;
    Status Commit();
    Status Abort();

  private:
    struct Impl;
    explicit WriteView(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend Result<WriteView> clWrite(Context&, GPtr, std::uint64_t);
    friend Result<WriteView> clWriteRange(Context&, GPtr, std::uint64_t, std::uint64_t,
                                           std::uint64_t);
};

Result<std::unique_ptr<Context>> clInit(CxloomConfig config);
// On failure the context is retained; finish outstanding activity and retry.
Status clDestroy(std::unique_ptr<Context>& context);

Result<GPtr> clAlloc(Context& context, std::size_t bytes, std::size_t alignment);
Result<GPtr> clAlloc(Context& context, const AllocOptions& options);

// Current-interval cache hits use DRAM directly. After an acquire boundary,
// blocks are validated lazily; multi-block results need not share a point in time.
Result<ReadView> clRead(Context& context, GPtr object, std::uint64_t timeout_ms = 10000);
Result<ReadView> clReadRange(Context& context, GPtr object, std::uint64_t offset,
                             std::uint64_t bytes, std::uint64_t timeout_ms = 10000);

// Drop this host's cached blocks without invalidating already returned views.
Status clInvalidate(Context& context, GPtr object);
// Pair these with application synchronization: release before publishing the
// synchronization event, acquire after observing it. Acquire rotates caches.
Status clSynchronizeRelease(Context& context);
Status clSynchronizeAcquire(Context& context);

// Commits publish blocks independently, including for full-object writes.
Result<WriteView> clWrite(Context& context, GPtr object, std::uint64_t timeout_ms = 10000);
Result<WriteView> clWriteRange(Context& context, GPtr object, std::uint64_t offset,
                               std::uint64_t bytes, std::uint64_t timeout_ms = 10000);

// Timeout leaves the object RETIRING; retry to finish the same transaction.
// All configured hosts must keep their progress pollers alive during reclamation.
Status clFree(Context& context, GPtr object);

}  // namespace cxloom
