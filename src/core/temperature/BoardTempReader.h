#pragma once

#ifdef TCMT_WINDOWS
#include <string>
#include <vector>
#include <cstdint>

struct BoardTemp {
    std::string name;
    double temperature;
};

// One Super I/O fan tachometer input. Boards label these CHA_FAN/SYS_FAN/...
// by their own wiring, so the name is the channel number the chip reports.
struct BoardFan {
    std::string name;
    int rpm;
};

class BoardTempReader {
public:
    // fansOut, when given, is filled from the same pass: the fan tachs live
    // behind the same banked window, and re-entering it costs another
    // detection sequence.
    static std::vector<BoardTemp> ReadAll(std::vector<BoardFan>* fansOut = nullptr);
    static bool IsAvailable();
};
#endif
