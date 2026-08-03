#include "lob/order_book.hpp"

#include <algorithm>

namespace lob {

void OrderBook::remove_maker(const Order& maker) {
    locations_.erase(maker.id);
}

std::vector<Trade> OrderBook::match_buy(OrderId taker, AccountId account,
                                        Price limit, Quantity& qty) {
    std::vector<Trade> trades;
    while (qty > 0 && !asks_.empty()) {
        auto level = asks_.begin();          // lowest ask
        if (level->first > limit) break;     // no cross
        auto& queue = level->second;

        while (qty > 0 && !queue.empty()) {
            Order& maker = queue.front();
            if (stp_ == StpPolicy::CancelResting && account != 0 &&
                maker.account == account) {
                remove_maker(maker);
                queue.pop_front();
                continue;
            }
            const Quantity fill = std::min(qty, maker.quantity);
            trades.push_back(Trade{taker, maker.id, level->first, fill});
            qty -= fill;
            maker.quantity -= fill;
            if (maker.quantity == 0) {
                remove_maker(maker);
                queue.pop_front();
            }
        }
        if (queue.empty()) asks_.erase(level);
    }
    return trades;
}

std::vector<Trade> OrderBook::match_sell(OrderId taker, AccountId account,
                                         Price limit, Quantity& qty) {
    std::vector<Trade> trades;
    while (qty > 0 && !bids_.empty()) {
        auto level = std::prev(bids_.end()); // highest bid
        if (level->first < limit) break;     // no cross
        auto& queue = level->second;

        while (qty > 0 && !queue.empty()) {
            Order& maker = queue.front();
            if (stp_ == StpPolicy::CancelResting && account != 0 &&
                maker.account == account) {
                remove_maker(maker);
                queue.pop_front();
                continue;
            }
            const Quantity fill = std::min(qty, maker.quantity);
            trades.push_back(Trade{taker, maker.id, level->first, fill});
            qty -= fill;
            maker.quantity -= fill;
            if (maker.quantity == 0) {
                remove_maker(maker);
                queue.pop_front();
            }
        }
        if (queue.empty()) bids_.erase(level);
    }
    return trades;
}

void OrderBook::rest_order(OrderId id, Side side, Price price, Quantity qty,
                           AccountId account) {
    Book& book = (side == Side::Buy) ? bids_ : asks_;
    auto level_it = book.try_emplace(price).first;
    auto& queue = level_it->second;
    queue.push_back(Order{id, side, OrderType::Limit, price, qty, next_seq_++,
                          account});
    locations_.emplace(id,
                       Location{side, level_it, std::prev(queue.end())});
}

SubmitResult OrderBook::add_limit(OrderId id, Side side, Price price,
                                  Quantity qty, AccountId account) {
    SubmitResult result;
    // Validate BEFORE mutating any state.
    if (qty == 0) {
        result.status = Reject::ZeroQty;
        return result;
    }
    if (price <= 0 || price > kMaxPrice) {
        result.status = Reject::BadPrice;
        return result;
    }
    if (locations_.find(id) != locations_.end()) {
        result.status = Reject::DuplicateId;
        return result;
    }

    Quantity remaining = qty;
    result.trades = (side == Side::Buy)
                        ? match_buy(id, account, price, remaining)
                        : match_sell(id, account, price, remaining);
    if (remaining > 0) {
        rest_order(id, side, price, remaining, account);
    }
    return result;
}

SubmitResult OrderBook::add_market(OrderId id, Side side, Quantity qty,
                                   AccountId account) {
    SubmitResult result;
    if (qty == 0) {
        result.status = Reject::ZeroQty;
        return result;
    }
    if (locations_.find(id) != locations_.end()) {
        result.status = Reject::DuplicateId;
        return result;
    }

    Quantity remaining = qty;
    if (side == Side::Buy) {
        // match any ask (all valid asks are <= kMaxPrice)
        result.trades = match_buy(id, account, kMaxPrice, remaining);
    } else {
        // match any bid (all valid bids are > 0 >= 0)
        result.trades = match_sell(id, account, 0, remaining);
    }
    // Any remainder is discarded (IOC). Market orders never rest.
    return result;
}

bool OrderBook::cancel(OrderId id) {
    auto it = locations_.find(id);
    if (it == locations_.end()) {
        return false;
    }
    const Location loc = it->second;
    auto& queue = loc.level->second;
    queue.erase(loc.order);
    if (queue.empty()) {
        (loc.side == Side::Buy ? bids_ : asks_).erase(loc.level);
    }
    locations_.erase(it);
    return true;
}

ModifyResult OrderBook::modify(OrderId id, Price new_price, Quantity new_qty) {
    ModifyResult result;
    auto it = locations_.find(id);
    if (it == locations_.end()) {
        result.found = false;
        return result;
    }
    result.found = true;
    Order& resting = *it->second.order;

    // Pure quantity decrease at the same price keeps time priority.
    if (new_price == resting.price && new_qty > 0 &&
        new_qty <= resting.quantity) {
        resting.quantity = new_qty;
        return result; // no trades
    }

    // Otherwise re-queue. Validate the NEW parameters BEFORE removing the
    // existing order, so a rejected modify is atomic (the original stays put).
    if (new_qty != 0 && (new_price <= 0 || new_price > kMaxPrice)) {
        result.status = Reject::BadPrice;
        return result; // resting order left intact
    }
    const Side side = resting.side;
    const AccountId account = resting.account;
    cancel(id);
    if (new_qty == 0) {
        return result; // treated as a cancel
    }
    SubmitResult sr = add_limit(id, side, new_price, new_qty, account);
    result.status = sr.status;
    result.trades = std::move(sr.trades);
    return result;
}

std::optional<Price> OrderBook::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return std::prev(bids_.end())->first; // highest bid
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first; // lowest ask
}

std::optional<Price> OrderBook::spread() const {
    if (bids_.empty() || asks_.empty()) return std::nullopt;
    // Both operands are validated to (0, kMaxPrice], so this cannot overflow.
    return asks_.begin()->first - std::prev(bids_.end())->first;
}

Quantity OrderBook::quantity_at(Side side, Price price) const {
    const Book& book = (side == Side::Buy) ? bids_ : asks_;
    auto it = book.find(price);
    if (it == book.end()) return 0;
    Quantity total = 0;
    for (const auto& o : it->second) {
        // saturating add guards against pathological overflow
        if (total > UINT64_MAX - o.quantity) return UINT64_MAX;
        total += o.quantity;
    }
    return total;
}

std::size_t OrderBook::physical_order_count() const {
    std::size_t n = 0;
    for (const auto& [price, queue] : bids_) n += queue.size();
    for (const auto& [price, queue] : asks_) n += queue.size();
    return n;
}

} // namespace lob
