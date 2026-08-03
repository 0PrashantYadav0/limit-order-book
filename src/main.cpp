#include "lob/order_book.hpp"

#include <iostream>
#include <string>
#include <vector>

using namespace lob;

namespace {

const char* side_str(Side s) { return s == Side::Buy ? "BUY " : "SELL"; }

const char* reject_str(Reject r) {
    switch (r) {
        case Reject::None: return "accepted";
        case Reject::DuplicateId: return "REJECT duplicate-id";
        case Reject::BadPrice: return "REJECT bad-price";
        case Reject::ZeroQty: return "REJECT zero-qty";
    }
    return "?";
}

void print_trades(const std::vector<Trade>& trades) {
    for (const auto& t : trades) {
        std::cout << "    TRADE  taker=" << t.taker_id << " maker=" << t.maker_id
                  << "  " << t.quantity << " @ " << t.price << '\n';
    }
    if (trades.empty()) {
        std::cout << "    (no trade)\n";
    }
}

void print_top(const OrderBook& book) {
    std::cout << "    book: bid ";
    if (auto b = book.best_bid()) std::cout << *b; else std::cout << "-";
    std::cout << " / ask ";
    if (auto a = book.best_ask()) std::cout << *a; else std::cout << "-";
    if (auto s = book.spread()) std::cout << "  (spread " << *s << ")";
    std::cout << "  | resting orders: " << book.order_count() << '\n';
}

void add(OrderBook& book, OrderId id, Side side, Price px, Quantity qty) {
    std::cout << "> LIMIT " << side_str(side) << " id=" << id << "  " << qty
              << " @ " << px << '\n';
    auto res = book.add_limit(id, side, px, qty);
    std::cout << "    status: " << reject_str(res.status) << '\n';
    print_trades(res.trades);
    print_top(book);
}

} // namespace

int main() {
    std::cout << "=== Limit Order Book - Matching Engine Demo ===\n\n";
    OrderBook book;

    std::cout << "-- Building the resting book --\n";
    add(book, 1, Side::Buy, 100, 10);
    add(book, 2, Side::Buy, 99, 20);
    add(book, 3, Side::Sell, 102, 15);
    add(book, 4, Side::Sell, 103, 30);

    std::cout << "\n-- Aggressive BUY that crosses the spread (price-time priority) --\n";
    add(book, 5, Side::Sell, 101, 5);   // rests at 101
    add(book, 6, Side::Buy, 102, 12);   // hits 101 (5) then 102 (7)

    std::cout << "\n-- Partial fill leaves a remainder resting --\n";
    add(book, 7, Side::Buy, 103, 40);   // takes 102 then 103, remainder rests

    std::cout << "\n-- Input validation (rejections, no state change) --\n";
    std::cout << "> LIMIT BUY id=1 (duplicate) -> "
              << reject_str(book.add_limit(1, Side::Buy, 100, 5).status) << '\n';
    std::cout << "> LIMIT BUY id=90 price=-5 -> "
              << reject_str(book.add_limit(90, Side::Buy, -5, 5).status) << '\n';
    std::cout << "> LIMIT BUY id=91 qty=0 -> "
              << reject_str(book.add_limit(91, Side::Buy, 100, 0).status) << '\n';

    std::cout << "\n-- Market SELL sweeps the bids --\n";
    std::cout << "> MARKET SELL id=8  25\n";
    print_trades(book.add_market(8, Side::Sell, 25).trades);
    print_top(book);

    std::cout << "\n-- Cancel a resting order --\n";
    add(book, 9, Side::Buy, 98, 8);
    std::cout << "> CANCEL id=9  -> " << (book.cancel(9) ? "ok" : "not found")
              << '\n';
    print_top(book);

    std::cout << "\n-- Modify (price change re-queues and re-matches) --\n";
    add(book, 10, Side::Sell, 105, 10);
    std::cout << "> MODIFY id=10 -> price 99, qty 10 (drops to cross bid 99)\n";
    print_trades(book.modify(10, 99, 10).trades);
    print_top(book);

    std::cout << "\n-- Self-trade prevention (StpPolicy::CancelResting) --\n";
    OrderBook stp_book(StpPolicy::CancelResting);
    (void)stp_book.add_limit(1, Side::Sell, 100, 10, /*account=*/7);
    std::cout << "> BUY from same account 7 crosses own resting sell:\n";
    auto stp_res = stp_book.add_limit(2, Side::Buy, 100, 10, /*account=*/7);
    print_trades(stp_res.trades); // none: resting cancelled, no self-trade
    std::cout << "    resting orders after STP: " << stp_book.order_count()
              << " (own resting sell was cancelled)\n";

    std::cout << "\nDemo complete.\n";
    return 0;
}
