#include "lob/order_book.hpp"

#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using namespace lob;

namespace {

int g_checks = 0;
int g_failed = 0;
std::string g_current;

void begin(const std::string& name) {
    g_current = name;
    std::cout << "[ RUN  ] " << name << '\n';
}
void end() { std::cout << "[  OK  ] " << g_current << '\n'; }

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failed;                                                        \
            std::cout << "[ FAIL ] " << g_current << " : " << #cond            \
                      << "  (line " << __LINE__ << ")\n";                      \
        }                                                                      \
    } while (0)

void test_rest_and_top() {
    begin("rest orders and top-of-book");
    OrderBook b;
    CHECK(!b.best_bid().has_value());
    CHECK(b.add_limit(1, Side::Buy, 100, 10).accepted());
    CHECK(b.add_limit(2, Side::Buy, 101, 5).accepted());
    CHECK(b.add_limit(3, Side::Sell, 105, 8).accepted());
    CHECK(b.best_bid().value() == 101);
    CHECK(b.best_ask().value() == 105);
    CHECK(b.spread().value() == 4);
    CHECK(b.order_count() == 3);
    CHECK(b.physical_order_count() == 3);
    end();
}

void test_full_fill() {
    begin("full fill removes maker");
    OrderBook b;
    (void)b.add_limit(1, Side::Sell, 100, 10);
    auto trades = b.add_limit(2, Side::Buy, 100, 10).trades;
    CHECK(trades.size() == 1);
    CHECK(trades[0].quantity == 10);
    CHECK(trades[0].price == 100);
    CHECK(trades[0].maker_id == 1);
    CHECK(trades[0].taker_id == 2);
    CHECK(b.empty());
    end();
}

void test_partial_fill_rests() {
    begin("partial fill rests remainder");
    OrderBook b;
    (void)b.add_limit(1, Side::Sell, 100, 4);
    auto trades = b.add_limit(2, Side::Buy, 100, 10).trades;
    CHECK(trades.size() == 1);
    CHECK(trades[0].quantity == 4);
    CHECK(b.best_bid().value() == 100);
    CHECK(b.quantity_at(Side::Buy, 100) == 6);
    end();
}

void test_time_priority() {
    begin("price-time priority (FIFO at a level)");
    OrderBook b;
    (void)b.add_limit(1, Side::Sell, 100, 5);
    (void)b.add_limit(2, Side::Sell, 100, 5);
    auto trades = b.add_limit(3, Side::Buy, 100, 7).trades;
    CHECK(trades.size() == 2);
    CHECK(trades[0].maker_id == 1);
    CHECK(trades[0].quantity == 5);
    CHECK(trades[1].maker_id == 2);
    CHECK(trades[1].quantity == 2);
    CHECK(b.quantity_at(Side::Sell, 100) == 3);
    end();
}

void test_price_improvement() {
    begin("aggressor gets maker price improvement");
    OrderBook b;
    (void)b.add_limit(1, Side::Sell, 100, 10);
    auto trades = b.add_limit(2, Side::Buy, 105, 10).trades;
    CHECK(trades.size() == 1);
    CHECK(trades[0].price == 100);
    end();
}

void test_multi_level_sweep() {
    begin("limit order sweeps multiple ask levels");
    OrderBook b;
    (void)b.add_limit(1, Side::Sell, 100, 5);
    (void)b.add_limit(2, Side::Sell, 101, 5);
    (void)b.add_limit(3, Side::Sell, 102, 5);
    auto trades = b.add_limit(4, Side::Buy, 101, 8).trades;
    CHECK(trades.size() == 2);
    CHECK(trades[0].price == 100);
    CHECK(trades[0].quantity == 5);
    CHECK(trades[1].price == 101);
    CHECK(trades[1].quantity == 3);
    CHECK(b.quantity_at(Side::Sell, 101) == 2);
    CHECK(b.best_ask().value() == 101);
    end();
}

void test_market_order() {
    begin("market order fills and discards remainder (IOC)");
    OrderBook b;
    (void)b.add_limit(1, Side::Buy, 100, 5);
    (void)b.add_limit(2, Side::Buy, 99, 5);
    auto trades = b.add_market(3, Side::Sell, 100).trades;
    CHECK(trades.size() == 2);
    CHECK(trades[0].price == 100);
    CHECK(trades[1].price == 99);
    CHECK(b.empty());
    end();
}

void test_cancel() {
    begin("cancel removes resting order");
    OrderBook b;
    (void)b.add_limit(1, Side::Buy, 100, 10);
    (void)b.add_limit(2, Side::Buy, 100, 10);
    CHECK(b.cancel(1) == true);
    CHECK(b.cancel(999) == false);
    CHECK(b.quantity_at(Side::Buy, 100) == 10);
    CHECK(b.cancel(2) == true);
    CHECK(!b.best_bid().has_value());
    end();
}

void test_modify_decrease_keeps_priority() {
    begin("modify quantity-decrease keeps time priority");
    OrderBook b;
    (void)b.add_limit(1, Side::Sell, 100, 10);
    (void)b.add_limit(2, Side::Sell, 100, 10);
    auto m = b.modify(1, 100, 4);
    CHECK(m.found);
    CHECK(m.trades.empty());
    auto trades = b.add_limit(3, Side::Buy, 100, 5).trades;
    CHECK(trades[0].maker_id == 1);
    CHECK(trades[0].quantity == 4);
    CHECK(trades[1].maker_id == 2);
    CHECK(trades[1].quantity == 1);
    end();
}

void test_modify_price_rematches() {
    begin("modify price change re-queues and can match");
    OrderBook b;
    (void)b.add_limit(1, Side::Sell, 105, 10);
    (void)b.add_limit(2, Side::Buy, 100, 10);
    auto m = b.modify(2, 105, 10);
    CHECK(m.found);
    CHECK(m.trades.size() == 1);
    CHECK(m.trades[0].price == 105);
    CHECK(b.empty());
    end();
}

void test_modify_not_found() {
    begin("modify of unknown id reports not-found");
    OrderBook b;
    auto m = b.modify(42, 100, 5);
    CHECK(!m.found);
    CHECK(m.trades.empty());
    end();
}

// ---- Robustness / rejection tests (the new hardening) --------------------

void test_duplicate_id_rejected() {
    begin("BLOCKER regression: duplicate id is rejected (no orphan)");
    OrderBook b;
    CHECK(b.add_limit(1, Side::Buy, 100, 10).accepted());
    auto dup = b.add_limit(1, Side::Buy, 100, 5); // same id
    CHECK(dup.status == Reject::DuplicateId);
    CHECK(dup.trades.empty());
    // No orphan: index and physical book agree, quantity unchanged.
    CHECK(b.order_count() == 1);
    CHECK(b.physical_order_count() == 1);
    CHECK(b.quantity_at(Side::Buy, 100) == 10);
    // Cancel fully clears the book (old bug left unkillable liquidity).
    CHECK(b.cancel(1));
    CHECK(b.empty());
    CHECK(b.physical_order_count() == 0);
    end();
}

void test_bad_price_rejected() {
    begin("non-positive / out-of-range price rejected");
    OrderBook b;
    CHECK(b.add_limit(1, Side::Buy, 0, 10).status == Reject::BadPrice);
    CHECK(b.add_limit(2, Side::Buy, -5, 10).status == Reject::BadPrice);
    CHECK(b.add_limit(3, Side::Buy, kMaxPrice + 1, 10).status ==
          Reject::BadPrice);
    CHECK(b.empty()); // nothing rested
    end();
}

void test_zero_qty_rejected() {
    begin("zero quantity rejected");
    OrderBook b;
    CHECK(b.add_limit(1, Side::Buy, 100, 0).status == Reject::ZeroQty);
    CHECK(b.add_market(2, Side::Sell, 0).status == Reject::ZeroQty);
    CHECK(b.empty());
    end();
}

void test_market_duplicate_id_rejected() {
    begin("market order with resting duplicate id rejected");
    OrderBook b;
    (void)b.add_limit(1, Side::Buy, 100, 10);
    CHECK(b.add_market(1, Side::Sell, 5).status == Reject::DuplicateId);
    CHECK(b.quantity_at(Side::Buy, 100) == 10); // untouched
    end();
}

void test_stp_cancel_resting() {
    begin("self-trade prevention cancels own resting order");
    OrderBook b(StpPolicy::CancelResting);
    (void)b.add_limit(1, Side::Sell, 100, 10, /*account=*/7);
    auto res = b.add_limit(2, Side::Buy, 100, 10, /*account=*/7);
    CHECK(res.accepted());
    CHECK(res.trades.empty());         // no self-trade
    CHECK(b.order_count() == 1);       // taker rested, maker was cancelled
    CHECK(b.best_bid().value() == 100);
    CHECK(!b.best_ask().has_value());
    end();
}

void test_stp_off_allows_cross_account() {
    begin("STP does not block different accounts");
    OrderBook b(StpPolicy::CancelResting);
    (void)b.add_limit(1, Side::Sell, 100, 10, /*account=*/7);
    auto res = b.add_limit(2, Side::Buy, 100, 10, /*account=*/8);
    CHECK(res.trades.size() == 1); // different account -> normal trade
    CHECK(b.empty());
    end();
}

void test_modify_bad_price_is_atomic() {
    begin("modify to invalid price is atomic (order preserved)");
    OrderBook b;
    (void)b.add_limit(1, Side::Buy, 100, 10);
    auto m = b.modify(1, -5, 10); // bad price
    CHECK(m.found);
    CHECK(m.status == Reject::BadPrice);
    CHECK(b.order_count() == 1);              // NOT destroyed
    CHECK(b.best_bid().value() == 100);
    auto m2 = b.modify(1, kMaxPrice + 1, 10); // out of range
    CHECK(m2.status == Reject::BadPrice);
    CHECK(b.order_count() == 1);
    CHECK(b.physical_order_count() == 1);
    end();
}

void test_stp_market_taker() {
    begin("self-trade prevention applies to market takers too");
    OrderBook b(StpPolicy::CancelResting);
    (void)b.add_limit(1, Side::Sell, 100, 10, /*account=*/7);
    auto res = b.add_market(2, Side::Buy, 10, /*account=*/7);
    CHECK(res.accepted());
    CHECK(res.trades.empty());   // no self-trade
    CHECK(b.empty());            // resting sell cancelled, market remainder IOC
    end();
}

// ---- Randomized property / fuzz test (would have caught BLOCKER-1) --------

void test_fuzz_invariants() {
    begin("randomized property test: invariants always hold");
    std::mt19937_64 rng(123456);
    OrderBook b;
    std::uniform_int_distribution<int> op(0, 3);
    std::uniform_int_distribution<Price> px(1, 40);
    std::uniform_int_distribution<Quantity> qd(1, 20);
    OrderId next = 1;
    bool ok = true;

    for (int i = 0; i < 5000; ++i) {
        const int o = op(rng);
        if (o <= 1 || next == 1) {
            const Side s = (rng() & 1) ? Side::Buy : Side::Sell;
            (void)b.add_limit(next++, s, px(rng), qd(rng));
        } else if (o == 2) {
            const Side s = (rng() & 1) ? Side::Buy : Side::Sell;
            (void)b.add_market(next++, s, qd(rng));
        } else {
            (void)b.cancel(1 + (rng() % next));
        }
        // Invariant 1: the id index and the physical book never disagree.
        if (b.order_count() != b.physical_order_count()) ok = false;
        // Invariant 2: the resting book is never crossed.
        if (b.best_bid() && b.best_ask() &&
            b.best_bid().value() >= b.best_ask().value())
            ok = false;
    }
    CHECK(ok);

    // Invariant 3: every id is cancelable; the book fully empties.
    for (OrderId id = 1; id < next; ++id) (void)b.cancel(id);
    CHECK(b.order_count() == 0);
    CHECK(b.physical_order_count() == 0);
    end();
}

} // namespace

int main() {
    std::cout << "=== OrderBook unit tests ===\n";
    test_rest_and_top();
    test_full_fill();
    test_partial_fill_rests();
    test_time_priority();
    test_price_improvement();
    test_multi_level_sweep();
    test_market_order();
    test_cancel();
    test_modify_decrease_keeps_priority();
    test_modify_price_rematches();
    test_modify_not_found();
    test_modify_bad_price_is_atomic();
    test_duplicate_id_rejected();
    test_bad_price_rejected();
    test_zero_qty_rejected();
    test_market_duplicate_id_rejected();
    test_stp_cancel_resting();
    test_stp_off_allows_cross_account();
    test_stp_market_taker();
    test_fuzz_invariants();

    std::cout << "\n" << (g_checks - g_failed) << "/" << g_checks
              << " checks passed.\n";
    if (g_failed == 0) {
        std::cout << "ALL TESTS PASSED\n";
        return 0;
    }
    std::cout << g_failed << " CHECK(S) FAILED\n";
    return 1;
}
