#include <atomic>
#include <cstring>
#include "cxloom/loompar/threading.h"

struct Context {
    cxloom::loompar::ThreadManager* manager;
    cxloom::GlobalThreadId id{};
    std::atomic<bool> checkpointed{false};
    std::atomic<bool> started{false};
    std::atomic<bool> requested{false};
};

static void Worker(void* bytes) {
    Context* context = nullptr;
    std::memcpy(&context, bytes, sizeof(context));
    context->started.store(true, std::memory_order_release);
    while (!context->requested.load(std::memory_order_acquire)) {}
    if (context->manager->MigrationSafePoint(context->id).ok())
        context->checkpointed.store(true, std::memory_order_release);
}

int main() {
    cxloom::loompar::ThreadManager manager(0);
    Context context{&manager};
    auto* pointer = &context;
    std::vector<std::byte> args(sizeof(pointer));
    std::memcpy(args.data(), &pointer, sizeof(pointer));
    auto id = manager.AllocateThread(0, 1, std::move(args));
    if (!id.ok()) return 1;
    context.id = id.value();
    if (!id.ok() || !manager.MarkLaunching(id.value()).ok() || !manager.Launch(id.value(), Worker).ok()) return 1;
    while (!context.started.load(std::memory_order_acquire)) {}
    if (!manager.RequestMigration(id.value(), 0).ok()) return 1;
    context.requested.store(true, std::memory_order_release);
    if (!manager.Join(id.value()).ok()) return 1;
    return context.checkpointed.load(std::memory_order_acquire) ? 0 : 1;
}
