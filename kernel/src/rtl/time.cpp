// System time <-> calendar fields (100 ns units since 1601-01-01, Gregorian calendar).
#include <xk/types.h>

typedef struct _TIME_FIELDS {
    SHORT Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday;
} TIME_FIELDS, *PTIME_FIELDS;

static bool RtlpIsLeap(LONG y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static const UCHAR RtlpMonthDays[2][12] = {{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
                                           {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}};

XBAPI void NTAPI RtlTimeToTimeFields(PLARGE_INTEGER Time, PTIME_FIELDS TimeFields) {
    ULONGLONG t = (ULONGLONG)Time->QuadPart;
    ULONG ms = (ULONG)((t / 10000) % 1000);
    ULONGLONG secs = t / 10000000;
    ULONG days = (ULONG)(secs / 86400), rest = (ULONG)(secs % 86400);
    TimeFields->Milliseconds = (SHORT)ms;
    TimeFields->Hour = (SHORT)(rest / 3600);
    TimeFields->Minute = (SHORT)(rest / 60 % 60);
    TimeFields->Second = (SHORT)(rest % 60);
    TimeFields->Weekday = (SHORT)((days + 1) % 7); // 1601-01-01 was a Monday
    LONG year = 1601;
    // 400-year cycles have 146097 days.
    year += 400 * (days / 146097);
    days %= 146097;
    for (;;) {
        ULONG n = RtlpIsLeap(year) ? 366 : 365;
        if (days < n)
            break;
        days -= n;
        year++;
    }
    int leap = RtlpIsLeap(year), month = 0;
    while (days >= RtlpMonthDays[leap][month])
        days -= RtlpMonthDays[leap][month++];
    TimeFields->Year = (SHORT)year;
    TimeFields->Month = (SHORT)(month + 1);
    TimeFields->Day = (SHORT)(days + 1);
}

XBAPI BOOLEAN NTAPI RtlTimeFieldsToTime(PTIME_FIELDS TimeFields, PLARGE_INTEGER Time) {
    TIME_FIELDS f = *TimeFields;
    if (f.Year < 1601 || f.Month < 1 || f.Month > 12 || f.Day < 1 ||
        f.Day > RtlpMonthDays[RtlpIsLeap(f.Year)][f.Month - 1] || f.Hour < 0 || f.Hour > 23 || f.Minute < 0 ||
        f.Minute > 59 || f.Second < 0 || f.Second > 59 || f.Milliseconds < 0 || f.Milliseconds > 999)
        return FALSE;
    LONG y = f.Year - 1601;
    ULONG days = y * 365 + y / 4 - y / 100 + y / 400;
    int leap = RtlpIsLeap(f.Year);
    for (int m = 0; m < f.Month - 1; m++)
        days += RtlpMonthDays[leap][m];
    days += f.Day - 1;
    ULONGLONG secs = (ULONGLONG)days * 86400 + f.Hour * 3600 + f.Minute * 60 + f.Second;
    Time->QuadPart = (LONGLONG)(secs * 10000000 + (ULONGLONG)f.Milliseconds * 10000);
    return TRUE;
}
