#pragma once

#include <string>

/* Board identity for the status panel, read once at startup from the
 * device tree: the model string and the SoC serial number. Both stay
 * empty where there is no Raspberry Pi device tree (an x86 laptop runs
 * on ACPI, other ARM boards carry their own vendor model string), and
 * the UI then hides the labels entirely. */
struct BoardInfo {
    std::string model;
    std::string serial;
};

BoardInfo board_query();
