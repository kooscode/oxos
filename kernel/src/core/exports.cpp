// Exported kernel data that does not belong to a specific subsystem yet.
#include <xk/core.h>

// Same version as the reference kernel so titles that test it behave identically.
XBOX_KRNL_VERSION XboxKrnlVersion = {1, 0, 4627, 1};

XK_PERSIST UCHAR XboxEEPROMKey[16]; // the EEPROM key the 2BL hands over (copied in by core/startup.cpp)
