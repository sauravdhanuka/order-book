#pragma once

#include "matching_engine.h"
#include <string>
#include <iostream>

namespace ob {

// Parses order commands from CSV format:
//   LIMIT,BUY,150.25,100
//   MARKET,SELL,,50
//   CANCEL,,,,5
//   PRINT
class CsvParser {
public:
    explicit CsvParser(MatchingEngine& engine) : engine_(engine) {}

    // Process a single line, print trades/output to the given stream
    void process_line(const std::string& line, std::ostream& os);

    // Process all lines from an input stream
    void process_stream(std::istream& is, std::ostream& os);

private:
    MatchingEngine& engine_;

    void print_trades(const std::vector<Trade>& trades, std::ostream& os);
};

} // namespace ob
