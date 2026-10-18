"""A virtual Xbox 360-style gamepad through Linux uinput (no extra modules), for driving xemu's controller input.

xemu (SDL) sees it as an ordinary USB pad (045e:028e) and maps it onto an emulated Xbox controller port.
"""
import fcntl, os, struct, time

EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
BUTTONS = {"A": 0x130, "B": 0x131, "X": 0x133, "Y": 0x134, "LB": 0x136, "RB": 0x137, "BACK": 0x13A,
           "START": 0x13B, "GUIDE": 0x13C, "LS": 0x13D, "RS": 0x13E}
# axis: (code, min, max)
AXES = {"LX": (0x00, -32768, 32767), "LY": (0x01, -32768, 32767), "LT": (0x02, 0, 255),
        "RX": (0x03, -32768, 32767), "RY": (0x04, -32768, 32767), "RT": (0x05, 0, 255),
        "HATX": (0x10, -1, 1), "HATY": (0x11, -1, 1)}
UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_ABSBIT = 0x40045564, 0x40045565, 0x40045567
UI_DEV_CREATE, UI_DEV_DESTROY = 0x5501, 0x5502
VPAD_VENDOR, VPAD_PRODUCT = 0x045E, 0x028E  # an Xbox 360 pad: SDL maps it without a mapping string


class VirtualPad:
    def __init__(self, name="Open Xbox Kernel test pad"):
        self.fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
        for ev in (EV_KEY, EV_ABS):
            fcntl.ioctl(self.fd, UI_SET_EVBIT, ev)
        for code in BUTTONS.values():
            fcntl.ioctl(self.fd, UI_SET_KEYBIT, code)
        absmax, absmin = [0] * 64, [0] * 64
        for code, lo, hi in AXES.values():
            fcntl.ioctl(self.fd, UI_SET_ABSBIT, code)
            absmin[code], absmax[code] = lo, hi
        dev = struct.pack("80sHHHHi", name.encode(), 3, VPAD_VENDOR, VPAD_PRODUCT, 0x0110, 0)
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
