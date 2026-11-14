// String port I/O, the HAL's READ_PORT_BUFFER_* and WRITE_PORT_BUFFER_* exports: Count reads or writes of one port,
// one after another.
#include <hardware/chipset/chipset.h>
#include <arch/arch.h>

XBAPI void NTAPI READ_PORT_BUFFER_UCHAR(PUCHAR Port, PUCHAR Buffer, ULONG Count) {
    for (ULONG i = 0; i < Count; i++)
        Buffer[i] = inb((USHORT)(ULONG)Port);
}

XBAPI void NTAPI READ_PORT_BUFFER_USHORT(PUSHORT Port, PUSHORT Buffer, ULONG Count) {
    for (ULONG i = 0; i < Count; i++)
        Buffer[i] = inw((USHORT)(ULONG)Port);
}

XBAPI void NTAPI READ_PORT_BUFFER_ULONG(PULONG Port, PULONG Buffer, ULONG Count) {
    for (ULONG i = 0; i < Count; i++)
        Buffer[i] = inl((USHORT)(ULONG)Port);
}

XBAPI void NTAPI WRITE_PORT_BUFFER_UCHAR(PUCHAR Port, PUCHAR Buffer, ULONG Count) {
    for (ULONG i = 0; i < Count; i++)
        outb((USHORT)(ULONG)Port, Buffer[i]);
}

XBAPI void NTAPI WRITE_PORT_BUFFER_USHORT(PUSHORT Port, PUSHORT Buffer, ULONG Count) {
    for (ULONG i = 0; i < Count; i++)
        outw((USHORT)(ULONG)Port, Buffer[i]);
}

XBAPI void NTAPI WRITE_PORT_BUFFER_ULONG(PULONG Port, PULONG Buffer, ULONG Count) {
    for (ULONG i = 0; i < Count; i++)
        outl((USHORT)(ULONG)Port, Buffer[i]);
}
