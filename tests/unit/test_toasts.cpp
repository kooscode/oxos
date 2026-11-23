// Toasts (spec/oxos_internal/36): the queue and its events (src/toasts/toast_queue.cpp), the box's size and place in each video mode and the YUY2 colours
// (src/toasts/toast_layout.h), and the in-game shortcuts' wait for their toast (kernel/extensions/oxos_igs/shortcuts.h, spec/oxos_internal/48 §3.4).
#include "kernel_test.h"

#include <string>
#include <vector>

#include <toasts/toast_layout.h>
#include <toasts/toasts.h>

#include "../../kernel/extensions/oxos_igs/shortcuts.h"

using namespace xk; // the kernel's subsystems as storage::, titles::, ...

namespace {

constexpr NTSTATUS STATUS_DEVICE_BUSY_ = (NTSTATUS)0x80000011, STATUS_CANCELLED_ = (NTSTATUS)0xC0000120, STATUS_NOT_SUPPORTED_ = (NTSTATUS)0xC00000BB,
                   STATUS_INVALID_PARAMETER_ = (NTSTATUS)0xC000000D;

// A toast with Text at the bottom centre.
OXOS_TOAST MakeToast(const char *Text, ULONG Seconds = 0) {
    return {Text, OXOS_TOAST_BOTTOM_CENTRE, Seconds, 0xDEAD};
}

} // namespace

TEST(Toasts, QueuedInOrderWithTheirOwnCopyOfTheText) {
    toasts::OpenToasts();
    char text[] = "first";
    OXOS_TOAST first = MakeToast(text), second = MakeToast("second\nline two", 3);
    ASSERT_EQ(toasts::EnqueueToast(first), STATUS_SUCCESS);
    ASSERT_EQ(toasts::EnqueueToast(second), STATUS_SUCCESS);
    EXPECT_NE(first.ToastId, 0u);
    EXPECT_NE(second.ToastId, first.ToastId);
    text[0] = 'X'; // the caller's buffer may change or go away at once
    toasts::QUEUED_TOAST taken;
    ASSERT_TRUE(toasts::TakeNextToast(taken));
    EXPECT_EQ(taken.ToastId, first.ToastId);
    EXPECT_STREQ(taken.Text, "first");
    EXPECT_EQ(taken.Seconds, toasts::TOAST_SECONDS_DEFAULT);
    EXPECT_EQ(taken.Owner, 0u);
    ASSERT_TRUE(toasts::TakeNextToast(taken));
    EXPECT_STREQ(taken.Text, "second\nline two");
    EXPECT_EQ(taken.Seconds, 3u);
    EXPECT_FALSE(toasts::TakeNextToast(taken));
    toasts::CloseToasts();
}

TEST(Toasts, AFullQueueSaysBusyAndAWaitTimesOut) {
    toasts::OpenToasts();
    for (ULONG i = 0; i < toasts::TOAST_QUEUE_LENGTH; i++) {
        OXOS_TOAST toast = MakeToast("one of eight");
        ASSERT_EQ(toasts::EnqueueToast(toast), STATUS_SUCCESS);
    }
    OXOS_TOAST more = MakeToast("ninth");
    EXPECT_EQ(toasts::EnqueueToast(more), STATUS_DEVICE_BUSY_);
    EXPECT_EQ(more.ToastId, 0u);
    EXPECT_EQ(toasts::EnqueueToastWait(more, 0), STATUS_TIMEOUT);
    toasts::QUEUED_TOAST taken;
    ASSERT_TRUE(toasts::TakeNextToast(taken));
    EXPECT_EQ(toasts::EnqueueToastWait(more, 0), STATUS_SUCCESS) << "a place freed";
    toasts::CloseToasts();
}

TEST(Toasts, WrongToastsAreRefused) {
    toasts::OpenToasts();
    const std::string tooLong(OXOS_TOAST_LINE_MAX + 1, 'x'), longest(OXOS_TOAST_LINE_MAX, 'x');
    const char *bad[] = {"", "three\nlines\nhere", "\nleading", "trailing\n", "tab\there", tooLong.c_str()};
    for (const char *text : bad) {
        OXOS_TOAST toast = MakeToast(text);
        EXPECT_EQ(toasts::EnqueueToast(toast), STATUS_INVALID_PARAMETER_) << text;
    }
    OXOS_TOAST missing = MakeToast(nullptr), place = MakeToast("place"), seconds = MakeToast("time", 31), fits = MakeToast(longest.c_str());
    place.Place = (OXOS_TOAST_PLACE)9;
    EXPECT_EQ(toasts::EnqueueToast(missing), STATUS_INVALID_PARAMETER_);
    EXPECT_EQ(toasts::EnqueueToast(place), STATUS_INVALID_PARAMETER_);
    EXPECT_EQ(toasts::EnqueueToast(seconds), STATUS_INVALID_PARAMETER_);
    EXPECT_EQ(toasts::EnqueueToast(fits), STATUS_SUCCESS);
    toasts::CloseToasts();
}

TEST(Toasts, NothingQueuesWhenClosedOrSwitchedOff) {
    toasts::OpenToasts();
    OXOS_TOAST toast = MakeToast("queued");
    ASSERT_EQ(toasts::EnqueueToast(toast), STATUS_SUCCESS);
    toasts::CloseToasts();
    toasts::QUEUED_TOAST taken;
    EXPECT_FALSE(toasts::TakeNextToast(taken)) << "the title's toasts go with it";
    EXPECT_EQ(toasts::EnqueueToast(toast), STATUS_CANCELLED_);
    EXPECT_EQ(toasts::EnqueueToastWait(toast, 100), STATUS_CANCELLED_);
    toasts::OpenToasts();
    toasts::ToastsOff = true;
    EXPECT_EQ(toasts::EnqueueToast(toast), STATUS_NOT_SUPPORTED_);
    toasts::ToastsOff = false;
    toasts::CloseToasts();
}

TEST(Toasts, EventsGoToTheToastsOwner) {
    toasts::OpenToasts();
    OXOS_TOAST toast = MakeToast("events");
    ASSERT_EQ(toasts::EnqueueToast(toast), STATUS_SUCCESS);
    toasts::QUEUED_TOAST taken;
    ASSERT_TRUE(toasts::TakeNextToast(taken));
    toasts::NoteToastEvent(taken, OXOS_TOAST_SHOWN);
    toasts::NoteToastEvent(taken, OXOS_TOAST_HIDDEN);
    taken.Owner = toasts::TOAST_NO_OWNER;
    toasts::NoteToastEvent(taken, OXOS_TOAST_DROPPED); // nobody's: no event
    ULONG owner;
    OXOS_TOAST_NOTICE notice;
    ASSERT_TRUE(toasts::TakeToastEvent(owner, notice));
    EXPECT_EQ(owner, 0u);
    EXPECT_EQ(notice.ToastId, toast.ToastId);
    EXPECT_EQ(notice.Event, OXOS_TOAST_SHOWN);
    ASSERT_TRUE(toasts::TakeToastEvent(owner, notice));
    EXPECT_EQ(notice.Event, OXOS_TOAST_HIDDEN);
    EXPECT_FALSE(toasts::TakeToastEvent(owner, notice));
    toasts::CloseToasts();
}

TEST(Toasts, AFailedExtensionsToastsLoseTheirOwner) {
    toasts::OpenToasts();
    OXOS_TOAST toast = MakeToast("orphan");
    ASSERT_EQ(toasts::EnqueueToast(toast), STATUS_SUCCESS);
    toasts::ForgetToastOwner(0);
    toasts::QUEUED_TOAST taken;
    ASSERT_TRUE(toasts::TakeNextToast(taken));
    EXPECT_EQ(taken.Owner, toasts::TOAST_NO_OWNER);
    toasts::CloseToasts();
}

TEST(Toasts, AllowedOnceTheTitleHasSetItsVideoMode) {
    toasts::OpenToasts();
    KeTickCount = 10000;
    toasts::NoteDisplayModeSet(0x88070701); // the kernel's own screen, before the title: does not count
    EXPECT_FALSE(toasts::TitleSettled()) << "no title yet";
    toasts::NoteTitleStarted();
    KeTickCount = 10300;
    toasts::NoteDisplayModeSet(0x88070701);
    KeTickCount = 10799;
    EXPECT_FALSE(toasts::TitleSettled());
    KeTickCount = 10800;
    EXPECT_TRUE(toasts::TitleSettled()) << "half a second after the title's mode set";
    KeTickCount = 11000;
    toasts::NoteDisplayModeSet(0x88070701); // a second mode set: half a second again
    EXPECT_FALSE(toasts::TitleSettled());
    toasts::CloseToasts();
    toasts::OpenToasts();
    KeTickCount = 20000;
    toasts::NoteTitleStarted();
    KeTickCount = 21999;
    EXPECT_FALSE(toasts::TitleSettled());
    KeTickCount = 22000;
    EXPECT_TRUE(toasts::TitleSettled()) << "no mode set: 2 seconds after the start";
    toasts::CloseToasts();
}

// The toasts a title's mode sets give: none until VIDEO_MODE_TOAST_DELAY_MS after a set, then at most one.
static std::vector<std::string> ModeToasts(std::initializer_list<ULONG> Modes) {
    toasts::OpenToasts();
    toasts::NoteTitleStarted();
    std::vector<std::string> texts;
    toasts::QUEUED_TOAST taken;
    for (ULONG mode : Modes) {
        toasts::NoteDisplayModeSet(mode);
        KeTickCount += toasts::VIDEO_MODE_TOAST_DELAY_MS - 1;
        toasts::QueueVideoModeToast();
        EXPECT_FALSE(toasts::TakeNextToast(taken)) << "too early";
        KeTickCount += 1;
        toasts::QueueVideoModeToast();
        toasts::QueueVideoModeToast(); // once only
        if (toasts::TakeNextToast(taken)) {
            EXPECT_EQ(taken.Owner, toasts::TOAST_NO_OWNER);
            texts.push_back(taken.Text);
        }
        EXPECT_FALSE(toasts::TakeNextToast(taken));
    }
    toasts::CloseToasts();
    return texts;
}

TEST(Toasts, EachTitlesVideoModeNamed) {
    using V = std::vector<std::string>;
    toasts::NoteDisplayModeSet(0x880B0A02); // the kernel's own screen, before the title: not named
    EXPECT_EQ(ModeToasts({0x880B0A02}), V{"Video Mode: 720p"}) << "a title's first mode, even the one on screen before";
    EXPECT_EQ(ModeToasts({0x880B0A02}), V{"Video Mode: 720p"}) << "and the next title's";
    EXPECT_EQ(ModeToasts({0x88070701, 0x880B0A02, 0x880E0C03}), (V{"Video Mode: 480p", "Video Mode: 720p", "Video Mode: 1080i"}));
    EXPECT_EQ(ModeToasts({0x88070701, 0x88080801}), V{"Video Mode: 480p"}) << "480p at another width: not named again";
    EXPECT_EQ(ModeToasts({0x98080801}), V{"Video Mode: 480p 16:9"});
    EXPECT_EQ(ModeToasts({0x04010101}), V{"Video Mode: 480i"});
    EXPECT_EQ(ModeToasts({0x54030301}), V{"Video Mode: 576i 16:9"});
    EXPECT_EQ(ModeToasts({0xC0060601}), V{"Video Mode: VGA"});
    EXPECT_EQ(ModeToasts({}), V{}) << "no mode set";
    toasts::VideoModeToastOff = true;
    EXPECT_EQ(ModeToasts({0x880B0A02}), V{}) << "show_video_mode = no";
    toasts::VideoModeToastOff = false;
}

TEST(ToastLayout, TheBoxFitsItsTextInWholeOverlayRows) {
    toasts::TOAST_BOX one = toasts::MeasureToast("Resetting to the dashboard"); // 26 characters
    EXPECT_EQ(one.Width, 352u);                                              // 26 * 12 + 32 = 344, rounded up to 32
    EXPECT_EQ(one.Height, 44u);
    EXPECT_EQ(one.Width * 2 % 64, 0u) << "the row pitch is a multiple of 64 bytes";
    toasts::TOAST_BOX two = toasts::MeasureToast("Friend request: Name\nBLACK + WHITE to open");
    EXPECT_EQ(two.Height, 64u);
    EXPECT_EQ(two.Width, 288u); // 21 * 12 + 32 = 284
    const std::string longest(OXOS_TOAST_LINE_MAX, 'x');
    EXPECT_LE(toasts::MeasureToast(longest.c_str()).Width, 640u - 2 * 640 * 6 / 100);
}

TEST(ToastLayout, PlacedTheSameInEveryMode) {
    toasts::TOAST_BOX box = {352, 44};
    toasts::TOAST_PLACEMENT sd = toasts::PlaceToast(box, OXOS_TOAST_BOTTOM_CENTRE, 640, 480);
    EXPECT_EQ(sd.Width, 352u);
    EXPECT_EQ(sd.X, 144u);
    EXPECT_EQ(sd.Y, 480u - 48 - 44);
    toasts::TOAST_PLACEMENT hd = toasts::PlaceToast(box, OXOS_TOAST_BOTTOM_CENTRE, 1280, 720);
    EXPECT_EQ(hd.Width, 528u) << "scaled by 720 / 480";
    EXPECT_EQ(hd.Height, 66u);
    EXPECT_EQ(hd.X, (1280u - 528) / 2);
    EXPECT_EQ(hd.Y, 720u - 72 - 66);
    toasts::TOAST_PLACEMENT topLeft = toasts::PlaceToast(box, OXOS_TOAST_TOP_LEFT, 640, 480);
    EXPECT_EQ(topLeft.X, 38u);
    EXPECT_EQ(topLeft.Y, 48u);
    toasts::TOAST_PLACEMENT right = toasts::PlaceToast(box, OXOS_TOAST_MIDDLE_RIGHT, 720, 480);
    EXPECT_EQ(right.X + right.Width, 720u - 43);
    EXPECT_EQ(right.Y, (480u - 44) / 2);
}

TEST(ToastLayout, YUY2ColoursAreBT601) {
    EXPECT_EQ(toasts::ConvertToYUY2(0x000000, 0x000000), 0x80108010u); // black: Y 16, U V 128
    EXPECT_EQ(toasts::ConvertToYUY2(0xFFFFFF, 0xFFFFFF), 0x80EB80EBu); // white: Y 235
    ULONG green = toasts::ConvertToYUY2(toasts::TOAST_COLOR_FRAME, toasts::TOAST_COLOR_FRAME);
    EXPECT_GT(green & 0xFF, 0x80u) << "a bright green";
    EXPECT_LT((green >> 8) & 0xFF, 0x80u);
    EXPECT_LT(green >> 24, 0x80u);
}

TEST(InGameShortcuts, AShortcutActsItsLaunchDelayAfterItsToastShows) {
    oxos::igs::PENDING_SHORTCUT pending = {0, 7, 1000, false, 0, 1000};
    EXPECT_FALSE(oxos::igs::IsPendingShortcutDue(pending, 1999));
    EXPECT_TRUE(oxos::igs::IsPendingShortcutDue(pending, 2000)) << "no toast within a second: at once";
    pending.Shown = true;
    pending.ShownAt = 1300;
    EXPECT_FALSE(oxos::igs::IsPendingShortcutDue(pending, 1300)) << "in the very tick it showed";
    EXPECT_FALSE(oxos::igs::IsPendingShortcutDue(pending, 2299));
    EXPECT_TRUE(oxos::igs::IsPendingShortcutDue(pending, 2300));
    pending.ShownAt = 0xFFFFFF00; // KeTickCount wraps after 49 days
    EXPECT_FALSE(oxos::igs::IsPendingShortcutDue(pending, 0xFFFFFF00));
    EXPECT_TRUE(oxos::igs::IsPendingShortcutDue(pending, 0xFFFFFF00 + 1000));
    pending.Shortcut = -1;
    EXPECT_FALSE(oxos::igs::IsPendingShortcutDue(pending, 5000)) << "nothing waits";
}
