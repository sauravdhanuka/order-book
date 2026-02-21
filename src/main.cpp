#include "matching_engine.h"
#include "csv_parser.h"
#include <iostream>
#include <fstream>
#include <memory>

int main(int argc, char* argv[]) {
    try {
        auto engine = std::make_unique<ob::MatchingEngine>();
        ob::CsvParser parser(*engine);

        if (argc > 1) {
            std::ifstream file(argv[1]);
            if (!file.is_open()) {
                std::cerr << "Error: cannot open file '" << argv[1] << "'\n";
                return 1;
            }
            parser.process_stream(file, std::cout);
        } else {
            parser.process_stream(std::cin, std::cout);
        }
    } catch (const std::exception& e) {
        std::cerr << "UNHANDLED EXCEPTION: " << e.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "UNHANDLED UNKNOWN EXCEPTION\n";
        return 1;
    }

    return 0;
}
