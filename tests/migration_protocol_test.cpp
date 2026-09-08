#include <iostream>
#include "cxloom/loompar/migration.h"
#define CHECK(x) do { if (!(x)) { std::cerr << "failed " << __LINE__ << '\n'; return 1; } } while (0)
int main() {
    using cxloom::loompar::MigrationTransaction;
    MigrationTransaction tx(0, 16);
    CHECK(!tx.Begin(16).ok());
    CHECK(!tx.Begin(0).ok());
    auto first = tx.Begin(1); CHECK(first.ok());
    CHECK(!tx.Begin(2).ok());
    CHECK(!tx.Prepared(1, first.value()).ok());
    CHECK(!tx.Quiesced(2, first.value()).ok());
    CHECK(tx.Quiesced(0, first.value()).ok());
    CHECK(tx.Quiesced(0, first.value()).ok());
    CHECK(!tx.AcceptsCompletion(0, 0));
    CHECK(tx.Abort(first.value()).ok());
    CHECK(tx.owner() == 0 && tx.AcceptsCompletion(0, 0));
    auto second = tx.Begin(2); CHECK(second.ok() && second.value() > first.value());
    CHECK(!tx.Quiesced(0, first.value()).ok());
    CHECK(tx.Quiesced(0, second.value()).ok());
    CHECK(tx.Prepared(2, second.value()).ok());
    CHECK(tx.Prepared(2, second.value()).ok());
    CHECK(tx.Commit(second.value()).ok());
    CHECK(tx.Commit(second.value()).ok());
    CHECK(!tx.Abort(second.value()).ok());
    CHECK(!tx.AcceptsCompletion(0, 0));
    CHECK(!tx.AcceptsCompletion(2, first.value()));
    CHECK(tx.AcceptsCompletion(2, second.value()));
    CHECK(!tx.Resumed(1, second.value()).ok());
    CHECK(tx.Resumed(2, second.value()).ok());
    CHECK(tx.Begin(3).ok());
    std::cout << "migration commit, rollback, endpoint and epoch checks passed\n";
}
