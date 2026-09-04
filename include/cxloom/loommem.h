#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "cxloom/common/config.h"
#include "cxloom/common/status.h"
#include "cxloom/common/types.h"

namespace cxloom {

using GPtr = GlobalPointer;

enum class ReadConsistency { kPerBlock, kWholeRange };
enum class WriteAtomicity { kPerBlock, kWholeRange };

struct AllocOptions {
    std::size_t bytes {0};
    std::size_t alignment {0};
    CoherenceGranularity coherence_granularity {CoherenceGranularity::kObject};
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
                                               std::uint64_t, ReadConsistency);
    friend Result<class WriteView> clWrite(Context&, GPtr, std::uint64_t);
    friend Result<class WriteView> clWriteRange(Context&, GPtr, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, WriteAtomicity);
    friend Status clFree(Context&, GPtr);
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
                                         std::uint64_t, ReadConsistency);
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
                                           std::uint64_t, WriteAtomicity);
};

Result<std::unique_ptr<Context>> clInit(CxloomConfig config);
Status clDestroy(std::unique_ptr<Context>& context);

Result<GPtr> clAlloc(Context& context, std::size_t bytes, std::size_t alignment);
Result<GPtr> clAlloc(Context& context, const AllocOptions& options);

Result<ReadView> clRead(Context& context, GPtr object, std::uint64_t timeout_ms = 10000);
Result<ReadView> clReadRange(Context& context, GPtr object, std::uint64_t offset,
                             std::uint64_t bytes, std::uint64_t timeout_ms = 10000,
                             ReadConsistency consistency = ReadConsistency::kPerBlock);

Result<WriteView> clWrite(Context& context, GPtr object, std::uint64_t timeout_ms = 10000);
Result<WriteView> clWriteRange(Context& context, GPtr object, std::uint64_t offset,
                               std::uint64_t bytes, std::uint64_t timeout_ms = 10000,
                               WriteAtomicity atomicity = WriteAtomicity::kPerBlock);

Status clFree(Context& context, GPtr object);

}  // namespace cxloom
