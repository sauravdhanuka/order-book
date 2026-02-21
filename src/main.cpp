#include "matching_engine.h"
#include "csv_parser.h"
#include <iostream>
#include <fstream>

int main(int argc, char* argv[]) {
    ob::MatchingEngine engine;
    ob::CsvParser parser(engine);

    if (argc > 1) {
        // Read from file
        std::ifstream file(argv[1]);
        if (!file.is_open()) {
            std::cerr << "Error: cannot open file '" << argv[1] << "'\n";
            return 1;
        }
        parser.process_stream(file, std::cout);
    } else {
        // Read from stdin
        parser.process_stream(std::cin, std::cout);
    }

    return 0;
}
