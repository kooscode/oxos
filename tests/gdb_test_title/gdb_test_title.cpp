// OXOS gdb test title (nxdk): code with known names for tests/gdb_test.py, which drives the kernel's GDB stub (GDB=1 builds, kernel/src/debugger) over
// xemu's serial port. A function to break in, a variable to watch, floating-point work, a debug line per round, and an access violation in the fourth
// round. The names are C
// names (extern "C"), so the test finds them in the symbol table as _StepGdbTest and _GdbTestCounter.
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

extern "C" {
// Written and read by StepGdbTest: the target of the test's watchpoints.
volatile int GdbTestCounter;
// Halved and added to every round, after the counter: floating-point work, so from then on the thread owns the FPU and the stub reads its float registers
// from the processor (before it, from the thread's save area).
volatile float GdbTestAverage;

// One round: adds Value to the counter and the average, returns twice the counter. Not inlined, so the test's breakpoint at its address is hit every round.
__attribute__((noinline)) int StepGdbTest(int Value) {
    GdbTestCounter += Value;
    GdbTestAverage = GdbTestAverage * 0.5f + static_cast<float>(Value);
    return GdbTestCounter * 2;
}
}

int main() {
    for (int round = 0;; round++) {
        int result = StepGdbTest(round);
        DbgPrint("gdb test: round %d result %d\n", round, result);
        if (round == 3) {
            volatile int *nothing = reinterpret_cast<volatile int *>(0x10);
            *nothing = 1; // an access violation: the stub's exception stop
        }
        Sleep(100);
    }
}
