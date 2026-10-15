// Controller test title (nxdk, open source): shows every Xbox controller input live, with a checklist of the inputs
// seen so far, and keeps the same state in XkPadTest for tests/pad_test.py (found in RAM by its signature).
#include <hal/debug.h>
#include <hal/video.h>
#include <string.h>
#include <usbh_lib.h>
#include <windows.h>
#include <xid_driver.h>

#define SLOTS 4

typedef struct {
    unsigned int Present, Uid, Type, Reports;
    unsigned char Last[20];      // the last input report (xid_gamepad_in)
    unsigned short DigitalSeen;  // every digital button bit seen set
    unsigned char AnalogMax[8];  // A B X Y Black White LT RT: highest value seen
    short AxisMin[4], AxisMax[4]; // LX LY RX RY
} PadSlot;

volatile struct {
    char Magic[8]; // "XKPADTST"
    unsigned int Version, Self, Connects, Disconnects, Frames; // Self: this block's virtual address
    PadSlot Slot[SLOTS];
} XkPadTest = {{'X', 'K', 'P', 'A', 'D', 'T', 'S', 'T'}, 1};

static int SlotOf(xid_dev_t *dev) {
    for (int i = 0; i < SLOTS; i++)
        if (XkPadTest.Slot[i].Present && XkPadTest.Slot[i].Uid == dev->uid)
            return i;
    return -1;
}

static void ReadDone(UTR_T *utr) {
    xid_dev_t *dev = (xid_dev_t *)utr->context;
    if (utr->status < 0 || !dev)
        return;
    int s = SlotOf(dev);
    if (s >= 0 && utr->xfer_len >= sizeof(xid_gamepad_in)) {
        volatile PadSlot *p = &XkPadTest.Slot[s];
        const xid_gamepad_in *in = (const xid_gamepad_in *)utr->buff;
        for (int i = 0; i < 20; i++)
            p->Last[i] = utr->buff[i];
        p->DigitalSeen |= in->dButtons;
        const unsigned char *analog = &in->a;
        for (int i = 0; i < 8; i++)
            if (analog[i] > p->AnalogMax[i])
                p->AnalogMax[i] = analog[i];
        short axis[4] = {in->leftStickX, in->leftStickY, in->rightStickX, in->rightStickY};
        for (int i = 0; i < 4; i++) {
            if (axis[i] < p->AxisMin[i])
                p->AxisMin[i] = axis[i];
            if (axis[i] > p->AxisMax[i])
                p->AxisMax[i] = axis[i];
        }
        p->Reports++;
    }
    utr->xfer_len = 0;
    utr->bIsTransferDone = 0;
    usbh_int_xfer(utr);
}

static void Connected(xid_dev_t *dev, int status) {
    XkPadTest.Connects++;
    for (int i = 0; i < SLOTS; i++)
        if (!XkPadTest.Slot[i].Present) {
            volatile PadSlot *p = &XkPadTest.Slot[i];
            memset((void *)p, 0, sizeof(*p));
            p->Uid = dev->uid;
            p->Type = usbh_xid_get_type(dev);
            p->Present = 1;
            if (p->Type >> 8 == XID_TYPE_GAMECONTROLLER)
                usbh_xid_read(dev, 0, ReadDone);
            return;
        }
}

static void Disconnected(xid_dev_t *dev, int status) {
    XkPadTest.Disconnects++;
    int s = SlotOf(dev);
    if (s >= 0)
        XkPadTest.Slot[s].Present = 0;
}

static const char *const DigitalNames[8] = {"UP", "DOWN", "LEFT", "RIGHT", "START", "BACK", "LS", "RS"};
static const char *const AnalogNames[8] = {"A", "B", "X", "Y", "BLACK", "WHITE", "LT", "RT"};

static void Show(void) {
    debugResetCursor();
    debugPrint("Open Xbox Kernel - controller test         frame %-8u\n\n", XkPadTest.Frames);
    for (int s = 0; s < SLOTS; s++) {
        volatile PadSlot *p = &XkPadTest.Slot[s];
        if (!p->Present) {
            debugPrint("Controller %d: -                                                    \n\n\n\n\n", s + 1);
            continue;
        }
        const xid_gamepad_in *in = (const xid_gamepad_in *)p->Last;
        debugPrint("Controller %d: type %04x, %-8u reports                          \n", s + 1, p->Type, p->Reports);
        debugPrint("  now:");
        for (int i = 0; i < 8; i++)
            debugPrint(" %s:%-3u", AnalogNames[i], (&in->a)[i]);
        debugPrint("\n  now: buttons %04x  L(%6d,%6d) R(%6d,%6d)          \n", in->dButtons, in->leftStickX,
                   in->leftStickY, in->rightStickX, in->rightStickY);
        debugPrint("  seen:");
        for (int i = 0; i < 8; i++)
            debugPrint(" %s%s", AnalogNames[i], p->AnalogMax[i] >= 0x80 ? "+" : "-");
        for (int i = 0; i < 8; i++)
            debugPrint(" %s%s", DigitalNames[i], p->DigitalSeen & (1 << i) ? "+" : "-");
        debugPrint("\n  seen: sticks L%s%s%s%s R%s%s%s%s (+ = reached, - = not yet)      \n\n",
                   p->AxisMin[0] < -16000 ? "<" : ".", p->AxisMax[0] > 16000 ? ">" : ".",
                   p->AxisMax[1] > 16000 ? "^" : ".", p->AxisMin[1] < -16000 ? "v" : ".",
                   p->AxisMin[2] < -16000 ? "<" : ".", p->AxisMax[2] > 16000 ? ">" : ".",
                   p->AxisMax[3] > 16000 ? "^" : ".", p->AxisMin[3] < -16000 ? "v" : ".");
    }
}

int main(void) {
    XkPadTest.Self = (unsigned int)&XkPadTest;
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    debugClearScreen();
    usbh_core_init();
    usbh_xid_init();
    usbh_install_xid_conn_callback(Connected, Disconnected);
    for (;;) {
        usbh_pooling_hubs();
        XkPadTest.Frames++;
        if (XkPadTest.Frames % 4 == 0)
            Show();
        XVideoWaitForVBlank();
    }
}
