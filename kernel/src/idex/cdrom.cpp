// IDE DVD drive (spec/04 §3). Milestone 3c-3 fills this in.
#include <xk/idex.h>

void IdexCdRomInit() {}

// Fatal boot errors (spec/05 §6): the reference reports the code through the SMC and halts.
volatile ULONG IdexFatalErrorCode;
[[noreturn]] void IdexFatalError(ULONG Code) {
    IdexFatalErrorCode = Code;
    KeBugCheckEx(0x00000F00 /* XK fatal error */, Code, 0, 0, 0);
}
