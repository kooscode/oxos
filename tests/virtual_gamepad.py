"""A virtual Xbox 360-style gamepad through Linux uinput (no extra modules), for driving xemu's controller input.

xemu (SDL) sees it as an ordinary USB pad (045e:028e) and maps it onto an emulated Xbox controller port.
"""
import fcntl, os, struct, time

# Linux input events: event types, then the pad's buttons (key codes) and axes.
EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
BUTTONS = {"A": 0x130, "B": 0x131, "X": 0x133, "Y": 0x134, "LB": 0x136, "RB": 0x137, "BACK": 0x13A, "START": 0x13B, "GUIDE": 0x13C, "LS": 0x13D, "RS": 0x13E}
# axis: (code, min, max)
AXES = {"LX": (0x00, -32768, 32767), "LY": (0x01, -32768, 32767), "LT": (0x02, 0, 255),
        "RX": (0x03, -32768, 32767), "RY": (0x04, -32768, 32767), "RT": (0x05, 0, 255),
        "HATX": (0x10, -1, 1), "HATY": (0x11, -1, 1)}

# uinput ioctls and the device's USB identity.
UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_ABSBIT = 0x40045564, 0x40045565, 0x40045567
UI_DEV_CREATE, UI_DEV_DESTROY = 0x5501, 0x5502
VPAD_VENDOR, VPAD_PRODUCT = 0x045E, 0x028E  # an Xbox 360 pad: SDL maps it without a mapping string
# The name carries the process id: SDL's GUID holds a CRC of the name, so each test's pad has its own GUID, and its xemu binds only that pad, not the pad of
# a test running at the same time (another session's xemu took this one's presses, 2026-11-26).
VPAD_VERSION, VPAD_NAME, BUS_USB = 0x0110, f"Open Xbox Kernel test pad {os.getpid()}", 3


def sdl_guid(bus, vendor, product, version, name):
    """SDL's joystick GUID for a device, as xemu keeps controllers in [input.bindings]: bus, the CRC-16 of the name, vendor, product and version, each a
    little-endian 16-bit word followed by a zero word (the last two words zero)."""
    crc = 0
    for byte in name.encode():
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return struct.pack("<8H", bus, crc, vendor, 0, product, 0, version, 0).hex()


# The xemu.toml tables that put this process's virtual pad on controller port 1 and nothing on ports 2-4, with xemu's binding of newly found pads off.
# Without them xemu gives port 1 to the first pad it finds, which may be a real controller plugged into the PC (the virtual pad's presses then reach no
# port, 2026-11-15), and gives the free ports to any other pad, such as another test's virtual pad.
XEMU_PORT1_BINDING = ("\n[input]\nauto_bind = false\n\n[input.bindings]\nport1_driver = 'usb-xbox-gamepad'\n"
                      f"port1 = '{sdl_guid(BUS_USB, VPAD_VENDOR, VPAD_PRODUCT, VPAD_VERSION, VPAD_NAME)}'\nport2 = ''\nport3 = ''\nport4 = ''\n")


class VirtualPad:
    def __init__(self, name=VPAD_NAME):
        self.fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
        for ev in (EV_KEY, EV_ABS):
            fcntl.ioctl(self.fd, UI_SET_EVBIT, ev)
        for code in BUTTONS.values():
            fcntl.ioctl(self.fd, UI_SET_KEYBIT, code)
        absmax, absmin = [0] * 64, [0] * 64
        for code, lo, hi in AXES.values():
            fcntl.ioctl(self.fd, UI_SET_ABSBIT, code)
            absmin[code], absmax[code] = lo, hi
        dev = struct.pack("80sHHHHi", name.encode(), BUS_USB, VPAD_VENDOR, VPAD_PRODUCT, VPAD_VERSION, 0)
        dev += struct.pack("64i", *absmax) + struct.pack("64i", *absmin) + bytes(4 * 64 * 2)
        os.write(self.fd, dev)
        fcntl.ioctl(self.fd, UI_DEV_CREATE)
        time.sleep(0.5)

    def _event(self, type_, code, value):
        os.write(self.fd, struct.pack("qqHHi", 0, 0, type_, code, value))

    def set(self, control, value):
        """Button name -> 0/1, axis name -> raw value."""
        if control in BUTTONS:
            self._event(EV_KEY, BUTTONS[control], value)
        else:
            self._event(EV_ABS, AXES[control][0], value)
        self._event(EV_SYN, 0, 0)

    def close(self):
        fcntl.ioctl(self.fd, UI_DEV_DESTROY)
        os.close(self.fd)
