// The trainer file (.etm, the EvolutionX trainer module, spec/oxos_internal/37 §3): the offsets oxos_trainers.xos and its menu (app/) read. Every offset is
// from the start of the file; the header's fields are dwords except the format.
#pragma once

constexpr unsigned long ETM_SIZE = 0x00;         // the file's size
constexpr unsigned long ETM_FORMAT = 0x04;       // 1 (16 bits)
constexpr unsigned long ETM_SWITCHES = 0x0A;     // offset of the cheat switches: one byte per cheat, 1 = on
constexpr unsigned long ETM_TEXTS = 0x0E;        // offset of the text table: offsets of the trainer's name, its author, then each cheat's name, ending in 0
constexpr unsigned long ETM_TITLE_IDS = 0x12;    // offset of the three title IDs it is for (0 = unused)
constexpr unsigned long ETM_ENTRY = 0x16;        // offset of its code, where the dashboard calls it
constexpr unsigned long ETM_HELPER_ENTRY = 0x1A; // non-zero: a trainer that needs a dashboard's helper routines, and is entered here (not supported)
constexpr unsigned long ETM_HEADER_BYTES = 0x1E; // the header up to 0x1A's dword
constexpr unsigned long ETM_MAX_BYTES = 0x10000; // the largest trainer loaded (XBMC4Gamers keeps 15 KB for one)
constexpr unsigned long ETM_MAX_CHEATS = 256;    // the most cheats counted in the text table
