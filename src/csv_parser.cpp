#include "csv_parser.h"
#include <sstream>
#include <algorithm>

namespace ob {

void CsvParser::process_line(const std::string& line, std::ostream& os) {
    // Skip empty lines and comments
    if (line.empty() || line[0] == '#') return;

    // Trim whitespace
    std::string trimmed = line;
    trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
    trimmed.erase(trimmed.find_last_not_of(" \t\r\n") + 1);
    if (trimmed.empty()) return;

    // Split by comma
    std::vector<std::string> tokens;
    std::istringstream ss(trimmed);
    std::string token;
    while (std::getline(ss, token, ',')) {
        tokens.push_back(token);
    }

    if (tokens.empty()) return;

    std::string cmd = tokens[0];
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper);

    if (cmd == "PRINT") {
        engine_.book().print(os);
        return;
    }

    if (cmd == "CANCEL") {
        if (tokens.size() < 5) {
            os << "ERROR: CANCEL requires order_id as 5th field\n";
            return;
        }
        OrderId id = std::stoull(tokens[4]);
        if (engine_.cancel_order(id)) {
            os << "CANCELLED " << id << "\n";
        } else {
            os << "CANCEL_REJECT " << id << " (not found)\n";
        }
        return;
    }

    // LIMIT or MARKET order
    if (tokens.size() < 4) {
        os << "ERROR: expected TYPE,SIDE,PRICE,QTY\n";
        return;
    }

    OrderType type;
    if (cmd == "LIMIT") {
        type = OrderType::LIMIT;
    } else if (cmd == "MARKET") {
        type = OrderType::MARKET;
    } else {
        os << "ERROR: unknown command '" << cmd << "'\n";
        return;
    }

    std::string side_str = tokens[1];
    std::transform(side_str.begin(), side_str.end(), side_str.begin(), ::toupper);
    Side side;
    if (side_str == "BUY" || side_str == "B") {
        side = Side::BUY;
    } else if (side_str == "SELL" || side_str == "S") {
        side = Side::SELL;
    } else {
        os << "ERROR: unknown side '" << side_str << "'\n";
        return;
    }

    Price price = 0;
    if (type == OrderType::LIMIT) {
        if (tokens[2].empty()) {
            os << "ERROR: LIMIT order requires a price\n";
            return;
        }
        price = price_from_double(std::stod(tokens[2]));
    }

    Quantity qty = static_cast<Quantity>(std::stoul(tokens[3]));
    if (qty == 0) {
        os << "ERROR: quantity must be > 0\n";
        return;
    }

    auto trades = engine_.process_order(side, type, price, qty);
    print_trades(trades, os);
}

void CsvParser::process_stream(std::istream& is, std::ostream& os) {
    std::string line;
    while (std::getline(is, line)) {
        process_line(line, os);
    }
}

void CsvParser::print_trades(const std::vector<Trade>& trades, std::ostream& os) {
    for (const auto& t : trades) {
        os << "TRADE " << t.buyer_order_id
           << " " << t.seller_order_id
           << " " << price_to_string(t.price)
           << " " << t.quantity << "\n";
    }
}

} // namespace ob
