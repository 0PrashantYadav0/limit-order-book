#pragma once

#include "lob/order.hpp"

#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace lob {

// Largest accepted absolute price (keeps spread()/arithmetic overflow-free).
inline constexpr Price kMaxPrice = 1'000'000'000'000LL; // 1e12 ticks

// A price-time-priority limit order book with a matching engine.
//
// THREAD SAFETY: This class is NOT thread-safe. It is designed for a single
// writer (single-producer/single-consumer ingest). Guard externally or shard
// one book per thread if concurrent access is required.
//
// Design:
//   * A single std::map<Price, list<Order>> holds every level. Asks match from
//     begin() (lowest), bids from the last element (highest), so both best
//     prices are O(log P) to find and O(1) to peek via cached ends.
//   * Each price level is a std::list<Order> in FIFO (time) order: O(1) erase
//     with stable iterators.
//   * OrderId -> {level iterator, order iterator} index gives genuine O(1)
//     cancel/modify (no map lookup on the cancel path).
//
// Prices are integers (ticks); matching executes at the resting (maker) price,
// giving the aggressor any price improvement.
class OrderBook {
public:
    using Book = std::map<Price, std::list<Order>>;

    explicit OrderBook(StpPolicy stp = StpPolicy::None) : stp_(stp) {}

    // Submit a limit order. Rejects duplicate ids, non-positive/out-of-range
    // prices, and zero quantity BEFORE mutating any state.
    [[nodiscard]] SubmitResult add_limit(OrderId id, Side side, Price price,
                                         Quantity qty, AccountId account = 0);

    // Submit a market order (immediate-or-cancel; never rests).
    [[nodiscard]] SubmitResult add_market(OrderId id, Side side, Quantity qty,
                                          AccountId account = 0);

    // Cancel a resting order. Returns true if it existed. O(1).
    bool cancel(OrderId id);

    // Modify a resting order. A pure quantity *decrease* at the same price keeps
    // time priority; any other change re-queues (cancel + re-add) and may match.
    [[nodiscard]] ModifyResult modify(OrderId id, Price new_price,
                                      Quantity new_qty);

    [[nodiscard]] std::optional<Price> best_bid() const;
    [[nodiscard]] std::optional<Price> best_ask() const;
    [[nodiscard]] std::optional<Price> spread() const;

    [[nodiscard]] Quantity quantity_at(Side side, Price price) const;
    [[nodiscard]] std::size_t order_count() const { return locations_.size(); }
    [[nodiscard]] bool empty() const { return locations_.empty(); }

    // Test/verification hook: counts orders physically present in the book by
    // walking every level (independent of the id index).
    [[nodiscard]] std::size_t physical_order_count() const;

private:
    struct Location {
        Side side;
        Book::iterator level;
        std::list<Order>::iterator order;
    };

    std::vector<Trade> match_buy(OrderId taker, AccountId account,
                                 Price limit, Quantity& qty);
    std::vector<Trade> match_sell(OrderId taker, AccountId account,
                                  Price limit, Quantity& qty);
    void rest_order(OrderId id, Side side, Price price, Quantity qty,
                    AccountId account);
    void remove_maker(const Order& maker); // erase maker from id index

    Book bids_;
    Book asks_;
    std::unordered_map<OrderId, Location> locations_;
    Sequence next_seq_{0};
    StpPolicy stp_{StpPolicy::None};
};

} // namespace lob
