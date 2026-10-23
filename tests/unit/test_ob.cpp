// Object manager and executive objects: names, directories, handles, references, links, and the Nt object calls.
#include "kernel_test.h"

#include <atomic>
#include <string>
#include <vector>

extern volatile ULONG ExpPoolBlocks;
NTSTATUS ObpCreateSymbolicLink(POBJECT_STRING LinkName, POBJECT_STRING TargetName);
XBAPI NTSTATUS NTAPI NtOpenDirectoryObject(PHANDLE DirectoryHandle, POBJECT_ATTRIBUTES ObjectAttributes);
XBAPI NTSTATUS NTAPI NtClearEvent(HANDLE EventHandle);
XBAPI NTSTATUS NTAPI NtPulseEvent(HANDLE EventHandle, PLONG PreviousState);
XBAPI NTSTATUS NTAPI NtReleaseMutant(HANDLE MutantHandle, PLONG PreviousCount);
XBAPI NTSTATUS NTAPI NtCreateTimer(PHANDLE TimerHandle, POBJECT_ATTRIBUTES ObjectAttributes, TIMER_TYPE TimerType);
typedef void(NTAPI *PTIMER_APC_ROUTINE)(PVOID TimerContext, ULONG TimerLowValue, LONG TimerHighValue);
XBAPI NTSTATUS NTAPI NtSetTimerEx(HANDLE TimerHandle, PLARGE_INTEGER DueTime, PTIMER_APC_ROUTINE TimerApcRoutine,
                                  KPROCESSOR_MODE ApcMode, PVOID TimerContext, BOOLEAN WakeTimer, LONG Period,
                                  PBOOLEAN PreviousState);
XBAPI NTSTATUS NTAPI NtCancelTimer(HANDLE TimerHandle, PBOOLEAN CurrentState);
XBAPI NTSTATUS NTAPI NtSignalAndWaitForSingleObjectEx(HANDLE SignalHandle, HANDLE WaitHandle, KPROCESSOR_MODE WaitMode,
                                                      BOOLEAN Alertable, PLARGE_INTEGER Timeout);
XBAPI NTSTATUS NTAPI NtYieldExecution();

namespace {
constexpr NTSTATUS NAME_COLLISION = (NTSTATUS)0xC0000035, NAME_NOT_FOUND = (NTSTATUS)0xC0000034,
                   PATH_NOT_FOUND = (NTSTATUS)0xC000003A, NAME_EXISTS = (NTSTATUS)0x40000000,
                   INVALID_HANDLE = (NTSTATUS)0xC0000008, TYPE_MISMATCH = (NTSTATUS)0xC0000024;

struct Name {
    OBJECT_STRING s;
    OBJECT_ATTRIBUTES a;
    explicit Name(const char *path, ULONG attributes = OBJ_CASE_INSENSITIVE, HANDLE root = nullptr) {
        RtlInitAnsiString(&s, path);
        a = {root, &s, attributes};
    }
};

HANDLE MakeDirectory(const char *path, ULONG attributes = OBJ_CASE_INSENSITIVE) {
    Name n(path, attributes);
    HANDLE h = nullptr;
    EXPECT_EQ(NtCreateDirectoryObject(&h, &n.a), STATUS_SUCCESS) << path;
    return h;
}

HANDLE MakeEvent(const char *path, ULONG attributes = OBJ_CASE_INSENSITIVE, NTSTATUS expect = STATUS_SUCCESS) {
    HANDLE h = nullptr;
    if (path) {
        Name n(path, attributes);
        EXPECT_EQ(NtCreateEvent(&h, &n.a, NotificationEvent, FALSE), expect) << path;
    } else {
        EXPECT_EQ(NtCreateEvent(&h, nullptr, NotificationEvent, FALSE), expect);
    }
    return h;
}
} // namespace

TEST(Ob, NamedObjectsAreFoundCaseInsensitivelyAndLeaveWithTheirLastHandle) {
    HANDLE dir = MakeDirectory("\\ObTest1");
    HANDLE ev = MakeEvent("\\ObTest1\\Alpha");
    PVOID o = nullptr;
    Name upper("\\OBTEST1\\ALPHA");
    ASSERT_EQ(ObReferenceObjectByName(&upper.s, OBJ_CASE_INSENSITIVE, &ExEventObjectType, nullptr, &o), STATUS_SUCCESS);
    PVOID byHandle = nullptr;
    ASSERT_EQ(ObReferenceObjectByHandle(ev, &ExEventObjectType, &byHandle), STATUS_SUCCESS);
    EXPECT_EQ(o, byHandle);
    EXPECT_EQ(ObReferenceObjectByHandle(ev, &ObDirectoryObjectType, &byHandle), TYPE_MISMATCH);
    ObfDereferenceObject(o);
    ObfDereferenceObject(o);
    // Relative to a root directory handle.
    Name rel("alpha", OBJ_CASE_INSENSITIVE, dir);
    HANDLE again = nullptr;
    ASSERT_EQ(ObOpenObjectByName(&rel.a, &ExEventObjectType, nullptr, &again), STATUS_SUCCESS);
    NtClose(again);
    // Not permanent: closing the last handle removes the name.
    EXPECT_EQ(NtClose(ev), STATUS_SUCCESS);
    EXPECT_EQ(ObReferenceObjectByName(&upper.s, OBJ_CASE_INSENSITIVE, nullptr, nullptr, &o), NAME_NOT_FOUND);
    NtClose(dir);
}

TEST(Ob, CollisionsOpenIfAndBadPaths) {
    HANDLE dir = MakeDirectory("\\ObTest2");
    HANDLE a = MakeEvent("\\ObTest2\\Same");
    MakeEvent("\\ObTest2\\Same", OBJ_CASE_INSENSITIVE, NAME_COLLISION);
    HANDLE b = MakeEvent("\\ObTest2\\SAME", OBJ_CASE_INSENSITIVE | OBJ_OPENIF, NAME_EXISTS);
    PVOID oa, ob;
    ObReferenceObjectByHandle(a, nullptr, &oa);
    ObReferenceObjectByHandle(b, nullptr, &ob);
    EXPECT_EQ(oa, ob) << "OBJ_OPENIF returns the existing object";
    ObfDereferenceObject(oa);
    ObfDereferenceObject(ob);
    MakeEvent("\\NoSuchDirectory\\X", OBJ_CASE_INSENSITIVE, PATH_NOT_FOUND);
    MakeEvent("\\ObTest2\\Same\\Below", OBJ_CASE_INSENSITIVE, PATH_NOT_FOUND) ; // an event is no directory
    NtClose(b);
    NtClose(a);
    NtClose(dir);
}

TEST(Ob, DirectoriesListTheirEntries) {
    HANDLE dir = MakeDirectory("\\ObTest3");
    std::vector<HANDLE> evs;
    for (const char *n : {"\\ObTest3\\one", "\\ObTest3\\two", "\\ObTest3\\three"})
        evs.push_back(MakeEvent(n));
    Name n("\\ObTest3");
    HANDLE opened;
    ASSERT_EQ(NtOpenDirectoryObject(&opened, &n.a), STATUS_SUCCESS);
    std::vector<std::string> names;
    ULONG context = 0, needed = 0;
    alignas(4) char buffer[128];
    NTSTATUS st = NtQueryDirectoryObject(opened, buffer, sizeof(buffer), TRUE, &context, &needed);
    while (st == STATUS_SUCCESS) {
        auto info = (POBJECT_DIRECTORY_INFORMATION)buffer;
        names.emplace_back(info->Name.Buffer, info->Name.Length);
        EXPECT_EQ(info->Type, (ULONG)'vevE');
        st = NtQueryDirectoryObject(opened, buffer, sizeof(buffer), FALSE, &context, &needed);
    }
    EXPECT_EQ(st, (NTSTATUS)0x8000001A) << "STATUS_NO_MORE_ENTRIES";
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names, (std::vector<std::string>{"one", "three", "two"}));
    EXPECT_EQ(NtQueryDirectoryObject(opened, buffer, 10, TRUE, &context, &needed), (NTSTATUS)0xC0000023)
        << "buffer too small";
    for (HANDLE h : evs)
        NtClose(h);
    NtClose(opened);
    NtClose(dir);
}

TEST(Ob, PermanentObjectsStayUntilMadeTemporary) {
    HANDLE dir = MakeDirectory("\\ObTest4", OBJ_CASE_INSENSITIVE | OBJ_PERMANENT);
    HANDLE ev = MakeEvent("\\ObTest4\\Keep", OBJ_CASE_INSENSITIVE | OBJ_PERMANENT);
    PVOID o;
    ObReferenceObjectByHandle(ev, nullptr, &o);
    NtClose(ev);
    Name n("\\ObTest4\\Keep");
    PVOID found;
    ASSERT_EQ(ObReferenceObjectByName(&n.s, OBJ_CASE_INSENSITIVE, nullptr, nullptr, &found), STATUS_SUCCESS);
    EXPECT_EQ(found, o);
    ObfDereferenceObject(found);
    ObMakeTemporaryObject(o); // no handles left: leaves the namespace now
    EXPECT_EQ(ObReferenceObjectByName(&n.s, OBJ_CASE_INSENSITIVE, nullptr, nullptr, &found), NAME_NOT_FOUND);
    ObfDereferenceObject(o);
    PVOID d;
    ObReferenceObjectByHandle(dir, nullptr, &d);
    NtClose(dir);
    ObMakeTemporaryObject(d);
    ObfDereferenceObject(d);
}

TEST(Ob, ObjectsAreFreedWithTheirLastReference) {
    ULONG blocks = ExpPoolBlocks;
    HANDLE ev = MakeEvent(nullptr); // unnamed
    EXPECT_GT(ExpPoolBlocks, blocks);
    PVOID o;
    ObReferenceObjectByHandle(ev, nullptr, &o);
    NtClose(ev);
    EXPECT_GT(ExpPoolBlocks, blocks) << "still referenced";
    ObfDereferenceObject(o);
    EXPECT_EQ(ExpPoolBlocks, blocks) << "freed with the last reference";
}

TEST(Ob, HandleTableGrowsAndHandlesAreReused) {
    HANDLE ev = MakeEvent(nullptr);
    std::vector<HANDLE> dups;
    for (int i = 0; i < 700; i++) { // past one sub-table (64) and the built-in root table (8 x 64)
        HANDLE h;
        ASSERT_EQ(NtDuplicateObject(ev, &h, 0), STATUS_SUCCESS) << i;
        ASSERT_EQ((ULONG)h & 3, 0u);
        dups.push_back(h);
    }
    std::vector<HANDLE> sorted = dups;
    std::sort(sorted.begin(), sorted.end());
    EXPECT_EQ(std::unique(sorted.begin(), sorted.end()), sorted.end()) << "distinct handles";
    HANDLE freed = dups[350];
    EXPECT_EQ(NtClose(freed), STATUS_SUCCESS);
    EXPECT_EQ(NtClose(freed), INVALID_HANDLE) << "closed twice";
    HANDLE reused;
    NtDuplicateObject(ev, &reused, 0);
    EXPECT_EQ(reused, freed) << "the freed slot comes back first";
    dups[350] = reused;
    HANDLE moved;
    ASSERT_EQ(NtDuplicateObject(dups[0], &moved, 1 /*DUPLICATE_CLOSE_SOURCE*/), STATUS_SUCCESS);
    PVOID o;
    EXPECT_EQ(ObReferenceObjectByHandle(dups[0], nullptr, &o), INVALID_HANDLE);
    dups[0] = moved;
    for (HANDLE h : dups)
        EXPECT_EQ(NtClose(h), STATUS_SUCCESS);
    NtClose(ev);
    EXPECT_EQ(NtClose((HANDLE)0x00FFFFF0), INVALID_HANDLE);
    EXPECT_EQ(NtClose(nullptr), INVALID_HANDLE);
}

TEST(Ob, SymbolicLinksResolveAndReportTheirTarget) {
    HANDLE dir = MakeDirectory("\\ObTest5", OBJ_CASE_INSENSITIVE | OBJ_PERMANENT);
    HANDLE ev = MakeEvent("\\ObTest5\\Target", OBJ_CASE_INSENSITIVE | OBJ_PERMANENT);
    Name link("\\??\\Q:"), target("\\ObTest5");
    ASSERT_EQ(ObpCreateSymbolicLink(&link.s, &target.s), STATUS_SUCCESS);
    // Through the drive letter (the \?? fast path) and down into the target directory.
    Name via("\\??\\q:\\TARGET");
    PVOID o, direct;
    ASSERT_EQ(ObReferenceObjectByName(&via.s, OBJ_CASE_INSENSITIVE, &ExEventObjectType, nullptr, &o), STATUS_SUCCESS);
    ObReferenceObjectByHandle(ev, nullptr, &direct);
    EXPECT_EQ(o, direct);
    ObfDereferenceObject(o);
    ObfDereferenceObject(direct);
    // A link to a link-free path that is not a drive letter.
    Name link2("\\ObTest5\\ToTarget"), target2("\\ObTest5\\Target");
    ASSERT_EQ(ObpCreateSymbolicLink(&link2.s, &target2.s), STATUS_SUCCESS);
    ASSERT_EQ(ObReferenceObjectByName(&link2.s, OBJ_CASE_INSENSITIVE, &ExEventObjectType, nullptr, &o), STATUS_SUCCESS);
    ObfDereferenceObject(o);
    HANDLE lh;
    ASSERT_EQ(NtOpenSymbolicLinkObject(&lh, &link.a), STATUS_SUCCESS);
    char buf[64];
    OBJECT_STRING out = {0, sizeof(buf), buf};
    ULONG len = 0;
    ASSERT_EQ(NtQuerySymbolicLinkObject(lh, &out, &len), STATUS_SUCCESS);
    EXPECT_EQ(std::string(out.Buffer, out.Length), "\\ObTest5");
    EXPECT_EQ(len, 8u);
    OBJECT_STRING tiny = {0, 3, buf};
    EXPECT_EQ(NtQuerySymbolicLinkObject(lh, &tiny, &len), (NTSTATUS)0xC0000023);
    NtClose(lh);
    Name missing("\\??\\Z:"), nowhere("\\NoSuchThing");
    EXPECT_NE(ObpCreateSymbolicLink(&missing.s, &nowhere.s), STATUS_SUCCESS);
    NtClose(ev);
    NtClose(dir);
}

// --- Executive objects through handles

TEST(ExObjects, EventsThroughHandles) {
    HANDLE e = MakeEvent(nullptr);
    LONG prev = -1;
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    EXPECT_EQ(NtWaitForSingleObject(e, FALSE, &zero), STATUS_TIMEOUT);
    EXPECT_EQ(NtSetEvent(e, &prev), STATUS_SUCCESS);
    EXPECT_EQ(prev, 0);
    EXPECT_EQ(NtWaitForSingleObject(e, FALSE, &zero), STATUS_SUCCESS);
    EXPECT_EQ(NtClearEvent(e), STATUS_SUCCESS);
    EXPECT_EQ(NtWaitForSingleObject(e, FALSE, &zero), STATUS_TIMEOUT);
    EXPECT_EQ(NtPulseEvent(e, &prev), STATUS_SUCCESS);
    EXPECT_EQ(NtWaitForSingleObject(e, FALSE, &zero), STATUS_TIMEOUT);
    EXPECT_EQ(NtSetEvent((HANDLE)0x00FFFFF0, &prev), INVALID_HANDLE);
    NtClose(e);
}

TEST(ExObjects, SemaphoresAndMutantsThroughHandles) {
    HANDLE s, m;
    ASSERT_EQ(NtCreateSemaphore(&s, nullptr, 1, 2), STATUS_SUCCESS);
    LONG prev;
    EXPECT_EQ(NtReleaseSemaphore(s, 1, &prev), STATUS_SUCCESS);
    EXPECT_EQ(prev, 1);
    EXPECT_NE(NtReleaseSemaphore(s, 1, &prev), STATUS_SUCCESS) << "above the maximum";
    EXPECT_NE(NtCreateSemaphore(&s, nullptr, 3, 2), STATUS_SUCCESS) << "initial above maximum";
    ASSERT_EQ(NtCreateMutant(&m, nullptr, TRUE), STATUS_SUCCESS);
    EXPECT_EQ(NtReleaseMutant(m, &prev), STATUS_SUCCESS);
    EXPECT_EQ(prev, 0);
    EXPECT_NE(NtReleaseMutant(m, &prev), STATUS_SUCCESS) << "not owned";
    HANDLE both[2] = {s, m};
    EXPECT_EQ(NtWaitForMultipleObjectsEx(2, both, WaitAll, KernelMode, FALSE, nullptr), STATUS_SUCCESS);
    NtReleaseMutant(m, &prev);
    NtClose(s);
    NtClose(m);
}

TEST(ExObjects, TimersThroughHandles) {
    HANDLE t;
    ASSERT_EQ(NtCreateTimer(&t, nullptr, NotificationTimer), STATUS_SUCCESS);
    LARGE_INTEGER due = RelativeMs(5), zero;
    zero.QuadPart = 0;
    BOOLEAN state = TRUE;
    ASSERT_EQ(NtSetTimerEx(t, &due, nullptr, KernelMode, nullptr, FALSE, 0, &state), STATUS_SUCCESS);
    EXPECT_FALSE(state);
    EXPECT_EQ(NtWaitForSingleObject(t, FALSE, &zero), STATUS_TIMEOUT);
    EXPECT_EQ(NtWaitForSingleObject(t, FALSE, nullptr), STATUS_SUCCESS);
    NtSetTimerEx(t, &due, nullptr, KernelMode, nullptr, FALSE, 0, nullptr);
    EXPECT_EQ(NtCancelTimer(t, &state), STATUS_SUCCESS);
    EXPECT_FALSE(state);
    NtClose(t);
}

TEST(ExObjects, SignalAndWaitAndYield) {
    HANDLE a = MakeEvent(nullptr), b = MakeEvent(nullptr);
    std::atomic<int> seen{0};
    PVOID ao;
    ObReferenceObjectByHandle(a, nullptr, &ao);
    PKTHREAD t = Spawn([&] {
        KeWaitForSingleObject(ao, Executive, KernelMode, FALSE, nullptr);
        seen = 1;
    });
    SleepMs(1);
    LARGE_INTEGER to = RelativeMs(5);
    EXPECT_EQ(NtSignalAndWaitForSingleObjectEx(a, b, KernelMode, FALSE, &to), STATUS_TIMEOUT);
    Join(t);
    EXPECT_EQ(seen, 1);
    ObfDereferenceObject(ao);
    EXPECT_EQ(NtYieldExecution(), (NTSTATUS)0x40000024) << "STATUS_NO_YIELD_PERFORMED: nothing else ready";
    NtClose(a);
    NtClose(b);
}

TEST(ExObjects, YieldGivesTheCpuEvenToALowerPriorityThread) {
    // A thread spinning on NtYieldExecution (SwitchToThread) while a lower-priority thread does the work must let it
    // run (a zero-length delay would not: it only yields to equal or higher priorities).
    std::atomic<int> done{0};
    PKTHREAD low = Spawn([&] { done = 1; }, 4);
    int spins = 0;
    while (!done && spins < 1000) {
        NtYieldExecution();
        spins++;
    }
    EXPECT_EQ(done, 1) << "the priority-4 thread never ran";
    EXPECT_LT(spins, 1000);
    Join(low);
    EXPECT_EQ(KeGetCurrentThread()->Priority, KeGetCurrentThread()->BasePriority);
}
