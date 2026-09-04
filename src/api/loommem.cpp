#include "cxloom/loommem.h"

#include <utility>

#include "cxloom/loommem/runtime.h"

namespace cxloom {

struct Context::Impl {
    explicit Impl(CxloomConfig config) : runtime(std::move(config)) {}
    ~Impl() {
        if (active) {
            runtime.StopQueuePoller();
            runtime.Finalize();
        }
    }
    loommem::LoomMemRuntime runtime;
    bool active {false};
};

struct WriteView::Impl {
    enum class State { kActive, kCommitted, kAborted };
    std::shared_ptr<Context::Impl> context;
    loommem::WriteBuffer buffer;
    State state {State::kActive};
};

namespace {

loommem::ReadConsistency ToInternal(ReadConsistency consistency) {
    return consistency == ReadConsistency::kWholeRange ? loommem::ReadConsistency::kWholeRange
                                                        : loommem::ReadConsistency::kPerBlock;
}

loommem::WriteAtomicity ToInternal(WriteAtomicity atomicity) {
    return atomicity == WriteAtomicity::kWholeRange ? loommem::WriteAtomicity::kWholeRange
                                                     : loommem::WriteAtomicity::kPerBlock;
}

}  // namespace

Context::Context(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

Context::~Context() = default;

ReadView::ReadView(std::shared_ptr<const void> storage, const void* data, std::size_t size)
    : storage_(std::move(storage)), data_(data), size_(size) {}

const void* ReadView::data() const { return data_; }

std::size_t ReadView::size() const { return size_; }

WriteView::WriteView() = default;
WriteView::WriteView(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
WriteView::WriteView(WriteView&&) noexcept = default;
WriteView& WriteView::operator=(WriteView&&) noexcept = default;

WriteView::~WriteView() {
    if (impl_ != nullptr && impl_->state == Impl::State::kActive)
        Abort();
}

void* WriteView::data() {
    return impl_ == nullptr || impl_->state != Impl::State::kActive ? nullptr : impl_->buffer.data();
}

const void* WriteView::data() const {
    return impl_ == nullptr || impl_->state != Impl::State::kActive ? nullptr : impl_->buffer.data();
}

std::size_t WriteView::size() const {
    return impl_ == nullptr || impl_->state != Impl::State::kActive ? 0 : impl_->buffer.bytes();
}

Status WriteView::Commit() {
    if (impl_ == nullptr || impl_->state != Impl::State::kActive)
        return Status::FailedPrecondition("write view is not active");
    const auto status = impl_->context->runtime.ReleaseWriteBuffer(impl_->buffer);
    if (status.ok()) {
        impl_->state = Impl::State::kCommitted;
        impl_->context.reset();
    }
    return status;
}

Status WriteView::Abort() {
    if (impl_ == nullptr || impl_->state != Impl::State::kActive)
        return Status::FailedPrecondition("write view is not active");
    const auto status = impl_->context->runtime.AbortWriteBuffer(impl_->buffer);
    if (status.ok()) {
        impl_->state = Impl::State::kAborted;
        impl_->context.reset();
    }
    return status;
}

Result<std::unique_ptr<Context>> clInit(CxloomConfig config) {
    auto impl = std::make_shared<Context::Impl>(std::move(config));
    const auto initialize = impl->runtime.Initialize();
    if (!initialize.ok())
        return initialize;
    const auto poller = impl->runtime.StartQueuePoller();
    if (!poller.ok()) {
        impl->runtime.Finalize();
        return poller;
    }
    impl->active = true;
    return std::unique_ptr<Context>(new Context(std::move(impl)));
}

Status clDestroy(std::unique_ptr<Context>& context) {
    if (context == nullptr)
        return Status::InvalidArgument("cl context is null");
    if (context->impl_.use_count() != 1)
        return Status::FailedPrecondition("active write views must be committed or aborted before clDestroy");
    if (context->impl_ == nullptr || !context->impl_->active)
        return Status::FailedPrecondition("cl context is not active");
    const auto poller = context->impl_->runtime.StopQueuePoller();
    const auto finalize = context->impl_->runtime.Finalize();
    context->impl_->active = false;
    context.reset();
    return !poller.ok() ? poller : finalize;
}

Result<GPtr> clAlloc(Context& context, std::size_t bytes, std::size_t alignment) {
    if (context.impl_ == nullptr || !context.impl_->active)
        return Status::FailedPrecondition("cl context is not active");
    return context.impl_->runtime.AllocateShared(bytes, alignment);
}

Result<GPtr> clAlloc(Context& context, const AllocOptions& options) {
    if (context.impl_ == nullptr || !context.impl_->active)
        return Status::FailedPrecondition("cl context is not active");
    loommem::AllocationOptions internal {options.bytes, options.alignment, options.coherence_granularity,
                                         options.coherence_block_bytes};
    return context.impl_->runtime.AllocateShared(internal);
}

Result<ReadView> clRead(Context& context, GPtr object, std::uint64_t timeout_ms) {
    if (context.impl_ == nullptr || !context.impl_->active)
        return Status::FailedPrecondition("cl context is not active");
    auto snapshot = context.impl_->runtime.AcquireReadSnapshot(object, timeout_ms);
    if (!snapshot.ok())
        return snapshot.status();
    auto storage = std::shared_ptr<const void>(snapshot.value().storage, snapshot.value().data());
    return ReadView(std::move(storage), snapshot.value().data(), snapshot.value().bytes());
}

Result<ReadView> clReadRange(Context& context, GPtr object, std::uint64_t offset,
                             std::uint64_t bytes, std::uint64_t timeout_ms, ReadConsistency consistency) {
    if (context.impl_ == nullptr || !context.impl_->active)
        return Status::FailedPrecondition("cl context is not active");
    auto snapshot = context.impl_->runtime.AcquireReadRange(object, offset, bytes, timeout_ms,
                                                             ToInternal(consistency));
    if (!snapshot.ok())
        return snapshot.status();
    auto storage = std::shared_ptr<const void>(snapshot.value().storage, snapshot.value().data());
    return ReadView(std::move(storage), snapshot.value().data(), snapshot.value().bytes());
}

Result<WriteView> clWrite(Context& context, GPtr object, std::uint64_t timeout_ms) {
    if (context.impl_ == nullptr || !context.impl_->active)
        return Status::FailedPrecondition("cl context is not active");
    auto write = context.impl_->runtime.AcquireWriteBuffer(object, timeout_ms);
    if (!write.ok())
        return write.status();
    auto impl = std::make_unique<WriteView::Impl>();
    impl->context = context.impl_;
    impl->buffer = std::move(write.value());
    return WriteView(std::move(impl));
}

Result<WriteView> clWriteRange(Context& context, GPtr object, std::uint64_t offset,
                               std::uint64_t bytes, std::uint64_t timeout_ms, WriteAtomicity atomicity) {
    if (context.impl_ == nullptr || !context.impl_->active)
        return Status::FailedPrecondition("cl context is not active");
    auto write = context.impl_->runtime.AcquireWriteRange(object, offset, bytes, timeout_ms,
                                                           ToInternal(atomicity));
    if (!write.ok())
        return write.status();
    auto impl = std::make_unique<WriteView::Impl>();
    impl->context = context.impl_;
    impl->buffer = std::move(write.value());
    return WriteView(std::move(impl));
}

Status clFree(Context& context, GPtr object) {
    if (context.impl_ == nullptr || !context.impl_->active)
        return Status::FailedPrecondition("cl context is not active");
    return context.impl_->runtime.FreeShared(object);
}

}  // namespace cxloom
