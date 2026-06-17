#pragma once

#ifdef TCMT_WINDOWS
#include <string>
#include <vector>
#include <cstdint>

struct BoardTemp {
    std::string name;
    double temperature;
};

class BoardTempReader {
public:
    static std::vector<BoardTemp> ReadAll();
    static bool IsAvailable();
};
#endif
