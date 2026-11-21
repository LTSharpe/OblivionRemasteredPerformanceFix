// Other mods' hooks and the load stage (worker obseparity 2026-11-21), included by selftest.cpp.
//  - call-only look-ups (FindAllCallable / MatchesAtCallable): FName's TCHAR constructor in the mapped exe with another
//    mod's jump written over its entry, in each jump form (E9 to a relay stub, FF 25 [rip+0] abs, mov rax / mov r11 + jmp,
//    push + mov [rsp+4] + ret): FindAll finds nothing, FindAllCallable finds the entry; a jump longer than readFrom allows,
//    changed bytes without a jump, a jump into the exe's own code and two hooked copies are not taken;
//  - patch sites (WriteCode): over each foreign form, and over a site the jump covers from a few bytes before, WriteCode
//    refuses and leaves the bytes alone; a jump into our own module, a stub in anonymous memory that does not jump on, an
//    import-thunk form (FF 25 slot inside the image) and an ordinary site are written;
//  - the load stage: ClassifyLoadStage's rules; the thread census of this process (main thread, our own exe's threads).
// Every byte written into the mapped exe is put back.
#pragma once

#include <cctype>

namespace hooks_test {
// FName::FName(const TCHAR*, EFindName), as settings/hdr_menu.cpp looks for it
constexpr char kFNameCtor[] =
    "48 89 5C 24 08 57 48 83 EC 30 48 8B D9 48 89 54 24 20 33 C9 41 8B F8 4C 8B D2 44 8B C9 48 85 D2 74 ?? 0F B7 02 "
    "66 85 C0 74 ?? 66 0F 1F 44 00 00 48 83 C2 02 0F B7 C0 44 0B C8 0F B7 02 66 85 C0 75 ?? 41 F7 C1 80 FF FF FF "
    "0F 95 44 24 2C";

int OurDetour() { return 7; }

void Raw(uint8_t* at, const void* bytes, size_t n)
{
    DWORD old;
    VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
}

// The jump forms: `len` bytes written at `at`, ending in `target` (abs) or `stub` (E9 rel32)
size_t Form(int form, uint8_t* out, const uint8_t* at, const void* target, const uint8_t* stub)
{
    const uint64_t t = reinterpret_cast<uint64_t>(target);
    switch (form) {
    case 0: {  // E9 rel32 to a relay stub
        const int32_t d = static_cast<int32_t>(stub - (at + 5));
        out[0] = 0xE9;
        memcpy(out + 1, &d, 4);
        return 5;
    }
    case 1:  // FF 25 [rip+0] + abs64
        out[0] = 0xFF, out[1] = 0x25, out[2] = out[3] = out[4] = out[5] = 0;
        memcpy(out + 6, &t, 8);
        return 14;
    case 2:  // mov rax, imm64; jmp rax
        out[0] = 0x48, out[1] = 0xB8;
        memcpy(out + 2, &t, 8);
        out[10] = 0xFF, out[11] = 0xE0;
        return 12;
    case 3:  // mov r11, imm64; jmp r11
        out[0] = 0x49, out[1] = 0xBB;
        memcpy(out + 2, &t, 8);
        out[10] = 0x41, out[11] = 0xFF, out[12] = 0xE3;
        return 13;
    case 5:  // push rax; mov rax, imm64; xchg [rsp], rax; ret (PolyHook2's in-place form)
        out[0] = 0x50, out[1] = 0x48, out[2] = 0xB8;
        memcpy(out + 3, &t, 8);
        out[11] = 0x48, out[12] = 0x87, out[13] = 0x04, out[14] = 0x24, out[15] = 0xC3;
        return 16;
    default: {  // push lo32; mov dword [rsp+4], hi32; ret
        const uint32_t lo = static_cast<uint32_t>(t), hi = static_cast<uint32_t>(t >> 32);
        out[0] = 0x68;
        memcpy(out + 1, &lo, 4);
        out[5] = 0xC7, out[6] = 0x44, out[7] = 0x24, out[8] = 0x04;
        memcpy(out + 9, &hi, 4);
        out[13] = 0xC3;
        return 14;
    }
    }
}

const char* const kFormNames[] = {"E9 rel32 to a relay", "FF 25 [rip+0] abs64", "mov rax + jmp rax", "mov r11 + jmp r11",
                                  "push + mov [rsp+4] + ret", "push rax + mov rax + xchg [rsp] + ret"};
uint64_t g_hooksAfterRun = 0;  // [hook] count when Run ends: no other self-test may add one (zero refusals on the clean exe)

void Run(const GameModule& game)
{
    const Pattern p = ParsePattern(kFNameCtor);
    const auto plain = FindAll(game.text, p);
    if (plain.size() != 1) {
        Check(false, "hooks: FName's TCHAR constructor found once in the mapped exe (the hook tests need it)");
        return;
    }
    Check(ForeignHooksMet() == 0, "hooks: no [hook] line from the self-test's searches and installs before this test");
    uint8_t* const fn = plain[0];
    uint8_t saved[0x60];
    memcpy(saved, fn, sizeof saved);
    const void* const foreign = reinterpret_cast<const void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "Sleep"));
    // a relay stub near the exe (as detour libraries place them): FF 25 [rip+0] -> kernel32
    uint8_t* const stub = AllocNearGame(4096);
    Check(stub != nullptr, "hooks: relay stub memory near the mapped exe");
    if (!stub || !foreign) return;
    {
        uint8_t relay[14];
        Form(1, relay, stub, foreign, nullptr);
        memcpy(stub, relay, sizeof relay);
        stub[64] = 0xC3;  // a stub that does not jump on (ret): "anonymous memory", our own stubs look like this
    }
    char name[160];
    for (int form = 0; form < 6; ++form) {
        uint8_t code[16];
        const size_t len = Form(form, code, fn, foreign, stub);
        ForgetOwnWritesForTest();  // refusal checks below: no earlier write of ours here
        Raw(fn, code, len);
        snprintf(name, sizeof name, "hooks: %s over FName's entry: FindAll finds nothing", kFormNames[form]);
        Check(FindAll(game.text, p).empty(), name);
        const JumpInfo j = DecodeJump(fn);
        const uint8_t* end = nullptr;
        snprintf(name, sizeof name, "hooks: %s decoded (%zu bytes) and followed to kernel32", kFormNames[form], len);
        Check(j.length == len && ForeignJumpAt(fn, &end) && end == foreign, name);
        const auto hooked = FindAllCallable(game.text, p, "selftest FName");
        snprintf(name, sizeof name, "hooks: %s: FindAllCallable finds the hooked entry", kFormNames[form]);
        Check(hooked.size() == 1 && hooked[0] == fn, name);
        snprintf(name, sizeof name, "hooks: %s: MatchesAtCallable accepts the hooked entry, MatchesAt does not", kFormNames[form]);
        Check(MatchesAtCallable(fn, p, "selftest FName") && !MatchesAt(fn, p), name);
        // readFrom: the caller reads bytes at offset 6 (inside every form's overwritten bytes except none)
        snprintf(name, sizeof name, "hooks: %s: not taken when the caller reads bytes the jump overwrote", kFormNames[form]);
        Check(FindAllCallable(game.text, p, "selftest FName", 4).empty() && !MatchesAtCallable(fn, p, "selftest FName", 4), name);
        // patch sites: at the entry, and 3 bytes in (covered by the jump)
        uint8_t before[20];
        memcpy(before, fn, sizeof before);
        const uint8_t five[5] = {0xE9, 0, 0, 0, 0};
        snprintf(name, sizeof name, "hooks: %s: WriteCode refuses the entry and a covered site, bytes left alone", kFormNames[form]);
        Check(!WriteCode(fn, five) && !WriteCode(fn + 3, std::span<const uint8_t>(five, 2)) && memcmp(before, fn, sizeof before) == 0, name);
        char why[256];
        snprintf(name, sizeof name, "hooks: %s: ForeignHookOverlap names kernel32", kFormNames[form]);
        const bool named = ForeignHookOverlap(fn + 2, 1, why, sizeof why) != nullptr;
        for (char* c = why; *c; ++c) *c = static_cast<char>(tolower(static_cast<unsigned char>(*c)));
        Check(named && strstr(why, "kernel32.dll+0x") != nullptr, name);
        Raw(fn, saved, sizeof saved);
    }
    Check(memcmp(fn, saved, sizeof saved) == 0 && FindAll(game.text, p).size() == 1, "hooks: FName's entry put back");
    // not taken: changed bytes without a jump; a jump into the exe's own code; two hooked copies
    {
        uint8_t junk[5] = {0x90, 0x90, 0x90, 0x90, 0x90};
        Raw(fn, junk, 5);
        Check(FindAllCallable(game.text, p, "selftest FName").empty() && !MatchesAtCallable(fn, p, "selftest FName"),
              "hooks: changed entry bytes without a jump: not taken");
        uint8_t code[16];
        Form(2, code, fn, game.text.begin + 0x1000, nullptr);  // mov rax, <exe code>; jmp rax
        Raw(fn, code, 12);
        Check(!ForeignJumpAt(fn) && FindAllCallable(game.text, p, "selftest FName").empty(),
              "hooks: a jump into the exe's own code is not another mod's hook");
        Raw(fn, saved, sizeof saved);
    }
    {
        // a second hooked copy: an RX buffer inside a section of our own search (a Section over the buffer and the copy)
        uint8_t* const buf = stub + 1024;
        memcpy(buf, fn, 96);
        memcpy(buf + 128, fn, 96);
        uint8_t code[16];
        const size_t len = Form(1, code, buf, foreign, nullptr);
        memcpy(buf, code, len);
        memcpy(buf + 128, code, len);
        const Section two{buf, buf + 256};
        Check(FindAllCallable(two, p, "selftest FName").empty(), "hooks: two hooked copies match: none used");
        memcpy(buf + 128, fn, 96);  // one hooked copy left, in anonymous memory (no module: any jump counts)
        const auto one = FindAllCallable(two, p, "selftest FName");
        Check(one.size() == 1 && one[0] == buf + 128, "hooks: an unhooked copy is found by the plain search first");
        memset(buf + 128, 0xCC, 1);  // the second copy changed but not hooked
        const auto onlyHooked = FindAllCallable(two, p, "selftest FName");
        Check(onlyHooked.size() == 1 && onlyHooked[0] == buf, "hooks: one hooked copy in anonymous memory is found");
    }
    // S2: another mod's anonymous executable memory (not ours) and a trampoline's resume point; S3: an FF 25 slot in a code cave
    {
        ForgetOwnWritesForTest();  // refusal checks below: no earlier write of ours here
        uint8_t* const theirs = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (theirs) {
            theirs[0] = 0x90, theirs[1] = 0xC3;  // a stub of theirs that does its own work
            uint8_t code[16];
            const size_t len = Form(1, code, fn, theirs, nullptr);
            Raw(fn, code, len);
            const uint8_t one = 0x90;
            Check(!WriteCode(fn + 2, std::span<const uint8_t>(&one, 1)) && memcmp(fn, code, len) == 0,
                  "hooks: a jump into another mod's anonymous executable memory refuses the write (only our own stubs are allowed)");
            uint8_t at20 = fn[20];
            Check(!WriteCode(fn + 20, std::span<const uint8_t>(&at20, 1)),
                  "hooks: a write just after another mod's jump (its trampoline's resume point) is refused");
            Check(WriteCode(fn + 40, std::span<const uint8_t>(fn + 40, 1)), "hooks: a write 40 bytes after it is allowed");
            Raw(fn, saved, sizeof saved);
            VirtualFree(theirs, 0, MEM_RELEASE);
        }
        // FF 25 through a slot inside .text (a code cave), the slot holding kernel32's address: followed, not an import thunk
        const uint64_t target = reinterpret_cast<uint64_t>(foreign);
        uint8_t cave[8];
        memcpy(cave, &target, 8);
        Raw(fn + 0x40, cave, 8);
        const int32_t d = static_cast<int32_t>((fn + 0x40) - (fn + 6));
        uint8_t code[6] = {0xFF, 0x25};
        memcpy(code + 2, &d, 4);
        Raw(fn, code, 6);
        const uint8_t* end = nullptr;
        Check(!DecodeJump(fn).importThunk && ForeignJumpAt(fn, &end) && end == foreign && !WriteCode(fn + 1, std::span<const uint8_t>(code + 1, 1)),
              "hooks: FF 25 through a slot in a code cave is followed (not an import thunk) and refuses the write");
        Raw(fn, saved, sizeof saved);
    }
    // WritePointer: a slot of the game's data already pointing to another mod's code is left alone
    {
        void** const slot = reinterpret_cast<void**>(game.data.begin + 0x100);
        void* const keep = *slot;
        void* const theirsPtr = const_cast<void*>(foreign);
        Raw(reinterpret_cast<uint8_t*>(slot), &theirsPtr, 8);
        Check(!WritePointer(slot, reinterpret_cast<void*>(&OurDetour)) && *slot == theirsPtr,
              "hooks: WritePointer refuses a game slot that already points to another module's code");
        void* const gameCode = game.text.begin + 0x1000;
        Raw(reinterpret_cast<uint8_t*>(slot), &gameCode, 8);
        Check(WritePointer(slot, reinterpret_cast<void*>(&OurDetour)) && *slot == reinterpret_cast<void*>(&OurDetour) &&
                  WritePointer(slot, keep) && *slot == keep,
              "hooks: WritePointer writes a game slot that points to the game's code (and to our own)");
        Raw(reinterpret_cast<uint8_t*>(slot), &keep, 8);
    }
    // patch sites that are written
    {
        uint8_t code[16];
        Form(1, code, fn, reinterpret_cast<const void*>(&OurDetour), nullptr);  // a jump into our own module
        Raw(fn, code, 14);
        Check(WriteCode(fn + 2, std::span<const uint8_t>(code + 2, 1)), "hooks: a site under a jump into our own module is written");
        Form(0, code, fn, nullptr, stub + 64);  // E9 to a stub that does not jump on
        Raw(fn, code, 5);
        Check(WriteCode(fn + 1, std::span<const uint8_t>(code + 1, 4)), "hooks: a site under a jump to a stub in anonymous memory is written");
        // FF 25 with its slot inside the image (an import thunk's form): the module's own code
        const int32_t d = static_cast<int32_t>((game.rdata.begin + 0x100) - (fn + 6));
        code[0] = 0xFF, code[1] = 0x25;
        memcpy(code + 2, &d, 4);
        Raw(fn, code, 6);
        Check(DecodeJump(fn).importThunk && !ForeignJumpAt(fn) && WriteCode(fn + 2, std::span<const uint8_t>(code + 2, 4)),
              "hooks: an import-thunk form (FF 25, slot in the image) is not a hook");
        Raw(fn, saved, sizeof saved);
        Check(WriteCode(fn, std::span<const uint8_t>(saved, 16)) &&
                  memcmp(fn, saved, sizeof saved) == 0,
              "hooks: an ordinary site is written (and the entry is back as it was)");
    }
    {
        // review 2, R3: writing back bytes we wrote (a rollback, a live switch) is never refused, also under another mod's jump
        ForgetOwnWritesForTest();
        const uint8_t b40 = fn[40];
        Check(WriteCode(fn + 40, std::span<const uint8_t>(&b40, 1)), "hooks: (a first write of ours at +40)");
        uint8_t code[16];
        Form(1, code, fn + 30, foreign, nullptr);
        Raw(fn + 30, code, 14);  // another mod's jump covering +30..+43 (not a function start: it must cover the site)
        Check(!WriteCode(fn + 34, std::span<const uint8_t>(fn + 34, 1)) && WriteCode(fn + 40, std::span<const uint8_t>(&b40, 1)),
              "hooks: under another mod's jump a new site is refused, our own written-back byte is not");
        Raw(fn, saved, sizeof saved);
        ForgetOwnWritesForTest();
        // review 2, R2: a jump that ends before the site counts only at a function start
        Form(1, code, fn + 8, foreign, nullptr);
        Raw(fn + 8, code, 14);  // not a function start, ends at +22
        Check(WriteCode(fn + 24, std::span<const uint8_t>(fn + 24, 1)),
              "hooks: a foreign-looking jump that ends before the site and is not at a function start is ignored");
        Raw(fn, saved, sizeof saved);
        ForgetOwnWritesForTest();
    }
    {
        // review 2, R1: OsPageCache's thunk patch before WinMain (hot-patch form) on fake thunks
        uint8_t* const buf = AllocNearGame(4096);
        if (buf) {
            const uint8_t thunk[9] = {0x48, 0x8B, 0xD1, 0x41, 0xB9, 0x04, 0x00, 0x00, 0x00};
            memset(buf, 0xCC, 16);
            memcpy(buf + 16, thunk, sizeof thunk);
            memset(buf + 64, 0x90, 16);  // a thunk without padding before it
            memcpy(buf + 80, thunk, sizeof thunk);
            buf[256] = 0xC3;
            const LoadStage was = CurrentLoadStage();
            SetLoadStage(LoadStage::BeforeWinMain);
            const bool hot = OsPageCacheTestPatchThunk(buf + 16, buf + 256);
            const bool none = !OsPageCacheTestPatchThunk(buf + 80, buf + 256);
            SetLoadStage(was);
            int32_t rel;
            memcpy(&rel, buf + 12, 4);
            Check(hot && buf[11] == 0xE9 && buf + 16 + rel == buf + 256 && buf[16] == 0xEB && buf[17] == 0xF9 && buf[18] == 0xD1 &&
                      memcmp(buf + 19, thunk + 3, 6) == 0,
                  "oscache: before WinMain the jump goes into the padding and a 2-byte EB F9 replaces the first instruction's start");
            Check(none && memcmp(buf + 80, thunk, sizeof thunk) == 0, "oscache: without 5 bytes of padding nothing is written");
            const uint8_t* const via = FollowJumps(buf + 16);
            Check(via == buf + 256, "oscache: the patched entry leads to the stub (EB F9, then the padding's E9)");
            FreeOwnCode(buf);
        }
    }
    Check(ForeignHooksMet() > 0, "hooks: [hook] lines counted");
    // WriteCodeAtomic: within one aligned qword only; the bytes around stay
    {
        uint8_t* const q = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(fn) + 7) & ~uintptr_t{7});
        uint8_t keep[24];
        memcpy(keep, q, sizeof keep);
        const uint8_t five[5] = {0xE9, 1, 2, 3, 4};
        Check(WriteCodeAtomicFits(q + 3, 5) && !WriteCodeAtomicFits(q + 4, 5) && !WriteCodeAtomicFits(q, 9),
              "hooks: WriteCodeAtomic fits only within one aligned 8-byte word");
        const bool wrote = WriteCodeAtomic(q + 3, five);
        Check(wrote && memcmp(q + 3, five, 5) == 0 && memcmp(q, keep, 3) == 0 && memcmp(q + 8, keep + 8, 16) == 0,
              "hooks: WriteCodeAtomic writes the bytes and keeps the rest of the word");
        Check(!WriteCodeAtomic(q + 4, five) && memcmp(q + 4, five + 1, 4) == 0, "hooks: WriteCodeAtomic refuses a straddling write");
        Raw(q, keep, sizeof keep);
        uint8_t code[16];
        const size_t len = Form(1, code, q, foreign, nullptr);
        ForgetOwnWritesForTest();  // refusal checks below: no earlier write of ours here
        Raw(q, code, len);
        Check(!WriteCodeAtomic(q + 2, std::span<const uint8_t>(five, 2)), "hooks: WriteCodeAtomic refuses over another mod's jump");
        Raw(q, keep, sizeof keep);
        Raw(fn, saved, sizeof saved);
    }
    Check(memcmp(fn, saved, sizeof saved) == 0, "hooks: the mapped exe is as it was");
    VirtualFree(stub, 0, MEM_RELEASE);
    g_hooksAfterRun = ForeignHooksMet();

    // load stage
    {
        const LoadStage was = CurrentLoadStage();
        SetLoadStage(LoadStage::BeforeWinMain);
        const bool set = CurrentLoadStage() == LoadStage::BeforeWinMain && strstr(LoadStageName(LoadStage::BeforeWinMain), "WinMain");
        SetLoadStage(was);
        Check(set && was == LoadStage::InDllMain, "stage: set and read back (the self-test runs as in DllMain)");
    }
    Check(ClassifyLoadStage(true, 0) == LoadStage::BeforeWinMain && ClassifyLoadStage(true, 1) == LoadStage::Late &&
              ClassifyLoadStage(false, 0) == LoadStage::Late,
          "stage: before WinMain only on the main thread with no exe thread");
    {
        const auto* self = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(self + reinterpret_cast<const IMAGE_DOS_HEADER*>(self)->e_lfanew);
        const uint8_t* const entry = self + nt->OptionalHeader.AddressOfEntryPoint;
        HANDLE go = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::thread t([go] { WaitForSingleObject(go, INFINITE); });
        const ThreadCensus c = TakeThreadCensus(self, nt->OptionalHeader.SizeOfImage, entry);
        SetEvent(go);
        t.join();
        CloseHandle(go);
        Check(c.ok && c.currentIsEntry && c.threads >= 2 && c.exeThreads >= 1,
              "stage: the census sees the main thread at the exe's entry and a thread started in the exe");
        const ThreadCensus g = TakeThreadCensus(game.base, game.sizeOfImage, game.base + 0x1000);
        Check(g.ok && !g.currentIsEntry && g.exeThreads == 0, "stage: no thread started in the mapped game exe");
        Check(ClassifyLoadStage(c.currentIsEntry, c.exeThreads) == LoadStage::Late, "stage: this process counts as late");
        // UE's crash-reporting thread proc (Steam build 19115871: its start, the thunk exe+0xF427C0, as seen in an OBSE64 game)
        if (game.timeDateStamp == 0xF19077A4u)
            Check(IsCrashReportingThreadStart(game.base, game.base + 0xF427C0) && !IsCrashReportingThreadStart(game.base, fn) &&
                      !IsCrashReportingThreadStart(game.base, game.text.begin),
                  "stage: the crash-reporting thread's start is recognised (and FName's constructor is not)");
        // S6: its signature (mov r9d,500 then a call through the WaitForMultipleObjects import) occurs once in all of .text,
        // so no other thread's start function (FRunnableThreadWin::_ThreadProc and the rest) can match
        {
            void** const wait = FindImportSlot(game.base, "KERNEL32.dll", "WaitForMultipleObjects");
            int sites = 0;
            for (uint8_t* s : FindAll(game.text, ParsePattern("41 B9 F4 01 00 00")))
                for (uint8_t* q = s + 6; q + 6 <= s + 26; ++q)
                    if (q[0] == 0xFF && q[1] == 0x15 && reinterpret_cast<void**>(RipTarget(q, 2, 6)) == wait) { ++sites; break; }
            Check(sites == 1, "stage: the crash thread's signature occurs once in .text (no other thread start matches)");
        }
    }
}
}  // namespace hooks_test
