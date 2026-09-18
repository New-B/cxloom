#include <atomic>
#include <cstdlib>
#include "cxloom/loompar/fiber.h"

int main() {
    std::atomic<int> phase{0};
    cxloom::loompar::Fiber* self = nullptr;
    cxloom::loompar::Fiber fiber(64 * 1024, [&] {
        phase.store(1);
        self->SafePoint();
        if (cxloom::loompar::Fiber::Current() != self) std::abort();
        phase.store(2);
    });
    self = &fiber;
    if (!fiber.Resume().ok() || phase != 1 || fiber.done()) return 1;
    if (cxloom::loompar::Fiber::Current() != nullptr) return 1;
    if (!fiber.TransferOwnership(7).ok() || fiber.owner() != 7) return 1;
    if (!fiber.Resume().ok() || phase != 2 || !fiber.done()) return 1;
    return fiber.Resume().code() == cxloom::StatusCode::kFailedPrecondition ? 0 : 1;
}
