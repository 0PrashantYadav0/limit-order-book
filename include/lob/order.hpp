#pragma once

#include <cstdint>
#include <vector>

namespace lob {

using OrderId = std::uint64_t;
using Price = std::int64_t;     // integer price in ticks (avoids float error)
using Quantity = std::uint64_t;
using Sequence = std::uint64_t; // monotonic, gives time priority
using AccountId = std::uint32_t; // 0 == "unspecified" (self-trade prevention off)

enum class Side : std::uint8_t { Buy, Sell };
enum class OrderType : std::uint8_t { Limit, Market };

// Self-trade-prevention policy applied when an aggressor would match a resting
// order from the same (non-zero) account.
enum class StpPolicy : std::uint8_t {
    None,          // no STP (default)
    CancelResting, // cancel the resting same-account order, keep aggressing
};

// Why a submission was rejected (None == accepted).
enum class Reject : std::uint8_t {
    None,
    DuplicateId, // an order with this id is already resting
    BadPrice,    // price <= 0 (or out of range)
    ZeroQty,     // quantity == 0
};

struct Order {
    OrderId id{};
    Side side{};
    OrderType type{};
    Price price{};        // ignored for Market orders
    Quantity quantity{};  // remaining quantity
    Sequence seq{};       // arrival order within a price level
    AccountId account{0}; // for self-trade prevention
};

// A fill produced by the matching engine.
struct Trade {
    OrderId taker_id{};
    OrderId maker_id{};
    Price price{};       // executes at the resting (maker) price
    Quantity quantity{};
};

// Result of add_limit / add_market.
struct SubmitResult {
    Reject status{Reject::None};
    std::vector<Trade> trades{};
    [[nodiscard]] bool accepted() const { return status == Reject::None; }
};

// Result of modify: distinguishes "not found" from "found, no trade".
struct ModifyResult {
    bool found{false};
    Reject status{Reject::None};
    std::vector<Trade> trades{};
};

} // namespace lob
