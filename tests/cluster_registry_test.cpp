#include "cxloom/loompar/cluster_registry.h"
#include <algorithm>
#include <iostream>
#define CHECK(x) do { if (!(x)) { std::cerr << "line " << __LINE__ << ": " << #x << std::endl; std::abort(); } } while (0)
using namespace cxloom;
using namespace cxloom::loompar;
ClusterFunction Function(std::string name, std::uintptr_t identity) {
    return {std::move(name), 1, 8, 0x10203040, identity, [](void*) {}};
}
int main() {
    auto a = std::vector<ClusterFunction>{Function("alpha", 1), Function("beta", 2)};
    auto b = std::vector<ClusterFunction>{Function("beta", 20), Function("alpha", 10)};
    ClusterFunctionRegistry first(0, 2), second(1, 2);
    CHECK(first.Wait(1).code() == StatusCode::kFailedPrecondition);
    auto outgoing = first.Install(a); CHECK(outgoing.ok());
    CHECK(first.Wait(1).code() == StatusCode::kUnavailable);
    // Remote publications can arrive before local bootstrap registration.
    for (const auto& message : outgoing.value()) CHECK(second.Handle(message).ok());
    auto reverse = second.Install(b); CHECK(reverse.ok());
    CHECK(second.Wait(100).ok());
    CHECK(first.Lookup(1).value() == second.Lookup(10).value());
    CHECK(!first.Resolve(first.Lookup(1).value(), 8, true).ok());
    CHECK(first.Resolve(first.Lookup(1).value(), 8, false).ok());
    std::reverse(reverse.value().begin(), reverse.value().end());
    for (const auto& message : reverse.value()) {
        CHECK(first.Handle(message).ok()); CHECK(first.Handle(message).ok());
    }
    CHECK(first.Wait(100).ok());
    CHECK(first.Install(a).value().empty());
    CHECK(first.Resolve(first.Lookup(1).value(), 8, true).ok());
    CHECK(first.Resolve(first.Lookup(1).value(), 7, true).status().code() == StatusCode::kInvalidArgument);
    CHECK(first.Lookup(99).status().code() == StatusCode::kNotFound);
    a[0].local_identity = 3;
    CHECK(first.Install(a).status().code() == StatusCode::kAlreadyExists);
    a[0].local_identity = 1;
    a[0].abi_version = 2;
    CHECK(first.Install(a).status().code() == StatusCode::kFailedPrecondition);
    a[0].abi_version = 1;
    for (int mismatch = 0; mismatch < 5; ++mismatch) {
        ClusterFunctionRegistry left(0, 2), right(1, 2);
        auto different = b;
        if (mismatch == 0) different.pop_back();
        if (mismatch == 1) different[0].abi_version++;
        if (mismatch == 2) different[0].schema_id++;
        if (mismatch == 3) different[0].argument_bytes++;
        if (mismatch == 4) different[0].name = "gamma";
        auto l = left.Install(a); auto r = right.Install(different); CHECK(l.ok() && r.ok());
        for (auto& message : l.value()) CHECK(right.Handle(message).ok());
        for (auto& message : r.value()) CHECK(left.Handle(message).ok());
        CHECK(left.Wait(100).code() == StatusCode::kFailedPrecondition);
        CHECK(right.Wait(100).code() == StatusCode::kFailedPrecondition);
    }
    // Conflicting duplicates fail closed, even after a successful rendezvous.
    ClusterFunctionRegistry conflicting(1, 2);
    auto changed = b; changed[0].schema_id++;
    auto conflict = conflicting.Install(changed); CHECK(conflict.ok());
    bool rejected = false;
    for (auto& message : conflict.value()) rejected |= !first.Handle(message).ok();
    CHECK(rejected && !first.Wait(1).ok());
    for (int invalid = 0; invalid < 7; ++invalid) {
        ClusterFunctionRegistry registry(0, 1);
        auto functions = a;
        if (invalid == 0) functions[0].name.clear();
        if (invalid == 1) functions[0].name.assign(64, 'a');
        if (invalid == 2) functions[0].abi_version = 0;
        if (invalid == 3) functions[0].schema_id = 0;
        if (invalid == 4) functions[0].argument_bytes = 81;
        if (invalid == 5) functions[0].local_identity = functions[1].local_identity;
        if (invalid == 6) functions[0].name = functions[1].name;
        CHECK(!registry.Install(functions).ok()); CHECK(!registry.installed());
        CHECK(registry.Install(a).ok()); CHECK(registry.Wait(100).ok());
    }
    ClusterFunctionRegistry empty0(0, 2), empty1(1, 2);
    auto e0 = empty0.Install({}), e1 = empty1.Install({});
    CHECK(e0.ok() && e1.ok());
    CHECK(empty0.Handle(e1.value()[0]).ok()); CHECK(empty1.Handle(e0.value()[0]).ok());
    CHECK(empty0.Wait(100).ok() && empty1.Wait(100).ok());
    std::cout << "manifest ordering, identity, timeout retry, ABI rejection and duplicate validation passed\n";
}
