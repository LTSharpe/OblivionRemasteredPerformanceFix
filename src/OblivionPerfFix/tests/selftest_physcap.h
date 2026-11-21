// PhysicsSubstepCap self-test (worker physkey 2026-11-20), included by selftest.cpp: on the mapped exe the reflection names
// the key looks up (UTF-8 in .rdata), FName's constructor and GUObjectArray by their patterns; then the game-thread rules on
// a fake object list (UE 5.3 layouts: FUObjectItem 24 bytes, UObjectBase flags +0x08 / index +0x0C / class +0x10 / name
// +0x18, UStruct super +0x40 / properties +0x50, FField class +0x08 / next +0x18 / name +0x20, FProperty dim +0x30 / size
// +0x34 / offset +0x44, FBoolProperty +0x70..+0x73) with a fake FName constructor: written only over the game's values
// (substepping on, 0.02, 6), MaxSubsteps 4 and 0.03 exactly as an ini line parses them, live off puts the game's back, values
// of anyone else (Engine.ini, later changes) and a Engine.ini line are left alone, a property of another type means nothing is
// written, a same-named object without the default-object flag or a dead one is skipped; the Engine.ini line reader.
#pragma once

namespace physcap_test {
struct Nm { uint32_t index, number; };
const wchar_t* const kNames[] = {L"", L"Default__PhysicsSettings", L"PhysicsSettings", L"bSubstepping", L"MaxSubstepDeltaTime",
                                 L"MaxSubsteps", L"BoolProperty", L"FloatProperty", L"IntProperty", L"PhysicsSettingsCore",
                                 L"MaxPhysicsDeltaTime"};
Nm* Fname(Nm* out, const wchar_t* name, int)
{
    out->index = out->number = 0;
    for (uint32_t i = 1; i < sizeof(kNames) / sizeof(kNames[0]); ++i)
        if (wcscmp(kNames[i], name) == 0) out->index = i;
    return out;
}
uint32_t Id(const wchar_t* name) { Nm n; Fname(&n, name, 0); return n.index; }
template <class T> void Put(uint8_t* p, size_t off, T v) { memcpy(p + off, &v, sizeof(T)); }
template <class T> T Get(const uint8_t* p, size_t off) { T v; memcpy(&v, p + off, sizeof(T)); return v; }

struct World {
    uint8_t array[0x40]{};
    uint8_t* chunks[1]{};
    uint8_t items[8 * 24]{};
    uint8_t fieldClass[3][16]{};  // Bool, Float, Int
    uint8_t props[4][0x80]{};     // MaxPhysicsDeltaTime, bSubstepping, MaxSubstepDeltaTime, MaxSubsteps
    uint8_t superCls[0x80]{}, cls[0x80]{};
    uint8_t obj[0x200]{}, decoy[0x200]{}, dead[0x200]{};
    static constexpr size_t kSub = 0x7A, kStep = 0x84, kSteps = 0x8C, kMaxDt = 0x40;
    void Prop(int i, const wchar_t* name, int type, int32_t size, int32_t off, uint8_t* next)
    {
        uint8_t* const f = props[i];
        Put<uint8_t*>(f, 0x08, fieldClass[type]);
        Put<uint8_t*>(f, 0x18, next);
        Put<uint64_t>(f, 0x20, Id(name));
        Put<int32_t>(f, 0x30, 1);
        Put<int32_t>(f, 0x34, size);
        Put<int32_t>(f, 0x44, off);
        if (type == 0) { f[0x70] = 1; f[0x71] = 0; f[0x72] = 0xFF; f[0x73] = 0xFF; }
    }
    void Object(uint8_t* o, int index, int32_t flags, int32_t itemFlags)
    {
        Put<int32_t>(o, 0x08, flags);
        Put<int32_t>(o, 0x0C, index);
        Put<uint8_t*>(o, 0x10, cls);
        Put<uint64_t>(o, 0x18, Id(L"Default__PhysicsSettings"));
        Put<uint8_t*>(items, 24 * index, o);
        Put<int32_t>(items, 24 * index + 8, itemFlags);
    }
    void Set(bool sub, float step, int32_t steps)
    {
        obj[kSub] = sub ? 1 : 0;
        Put(obj, kStep, step);
        Put(obj, kSteps, steps);
    }
    World(int stepsType = 2)
    {
        Put<uint64_t>(fieldClass[0], 0, Id(L"BoolProperty"));
        Put<uint64_t>(fieldClass[1], 0, Id(L"FloatProperty"));
        Put<uint64_t>(fieldClass[2], 0, Id(L"IntProperty"));
        // the class: MaxPhysicsDeltaTime -> bSubstepping -> MaxSubstepDeltaTime -> MaxSubsteps; its parent has none of them
        Prop(3, L"MaxSubsteps", stepsType, 4, kSteps, nullptr);
        Prop(2, L"MaxSubstepDeltaTime", 1, 4, kStep, props[3]);
        Prop(1, L"bSubstepping", 0, 1, kSub, props[2]);
        Prop(0, L"MaxPhysicsDeltaTime", 1, 4, kMaxDt, props[1]);
        Put<uint64_t>(superCls, 0x18, Id(L"PhysicsSettingsCore"));
        Put<uint64_t>(cls, 0x18, Id(L"PhysicsSettings"));
        Put<uint8_t*>(cls, 0x40, superCls);
        Put<uint8_t*>(cls, 0x50, props[0]);
        chunks[0] = items;
        Put<uint8_t**>(array, 0x10, chunks);
        Put<int32_t>(array, 0x24, 6);
        Object(decoy, 1, 0, 0);                  // the same name, no RF_ClassDefaultObject
        Object(dead, 2, 0x10, 1 << 28);          // the default-object flag, but being destroyed
        Object(obj, 4, 0x10 | 0x01, 0);
        Set(true, 0.02f, 6);
        Put(decoy, kStep, 0.02f); Put(decoy, kSteps, int32_t{6});
        Put(dead, kStep, 0.02f); Put(dead, kSteps, int32_t{6});
    }
};

bool PutFile(const wchar_t* path, const char* text)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const BOOL ok = ::WriteFile(h, text, static_cast<DWORD>(strlen(text)), &w, nullptr);
    CloseHandle(h);
    return ok != 0;
}
}  // namespace physcap_test

static void PhysCapRun()
{
    using namespace physcap_test;
    {   // the mapped exe: the reflection names (UTF-8, whole strings) and the two patterns
        const GameModule& game = Game();
        auto inRdata = [&](const char* s) {
            const size_t n = strlen(s) + 1;
            for (const uint8_t* p = game.rdata.begin + 1; p + n <= game.rdata.end; ++p)
                if (p[-1] == 0 && memcmp(p, s, n) == 0) return true;
            return false;
        };
        auto inRdata16 = [&](const wchar_t* s) {  // a whole UTF-16 string (the class's registered name)
            const size_t n = (wcslen(s) + 1) * 2;
            for (const uint8_t* p = game.rdata.begin + 2; p + n <= game.rdata.end; p += 2)
                if (p[-1] == 0 && p[-2] == 0 && memcmp(p, s, n) == 0) return true;
            return false;
        };
        Check(inRdata("MaxSubstepDeltaTime") && inRdata("MaxSubsteps") && inRdata("bSubstepping") && inRdata16(L"UPhysicsSettings"),
              "physcap: MaxSubstepDeltaTime, MaxSubsteps, bSubstepping (UTF-8) and UPhysicsSettings (UTF-16) are reflection names in the exe");
        Check(FindFNameCtor() != nullptr && FindObjectArrayFromDtor() != nullptr,
              "physcap: FName's constructor and GUObjectArray found by their patterns");
    }
    wchar_t tmp[MAX_PATH], noIni[MAX_PATH + 32], ini[MAX_PATH + 32];
    GetTempPathW(MAX_PATH, tmp);
    swprintf_s(noIni, L"%lsopf-physcap-none.ini", tmp);  // no such file
    swprintf_s(ini, L"%lsopf-physcap-engine.ini", tmp);
    DeleteFileW(noIni);
    void* const fname = reinterpret_cast<void*>(&Fname);
    {
        World w;
        PhysicsSubstepCapTestSetup(w.array, fname, noIni);
        PhysicsSubstepCapOnGameThread();
        Check(Get<float>(w.obj, World::kStep) == 0.02f && PhysicsSubstepCapSets() == 0, "physcap: key 0 (default): the game's values stay");
        SetPhysicsSubstepCap(1);
        PhysicsSubstepCapOnGameThread();
        Check(Get<float>(w.obj, World::kStep) == 0.03f && Get<int32_t>(w.obj, World::kSteps) == 4 && PhysicsSubstepCapSets() == 1,
              "physcap: key 1 over the game's 0.02 / 6: 0.03 / 4");
        Check(Get<float>(w.obj, World::kStep) == static_cast<float>(atof("0.03")),
              "physcap: 0.03 is the float an Engine.ini line \"0.03\" gives");
        Check(Get<float>(w.decoy, World::kStep) == 0.02f && Get<float>(w.dead, World::kStep) == 0.02f &&
              Get<float>(w.obj, World::kMaxDt) == 0.0f && w.obj[World::kSub] == 1,
              "physcap: the same-named non-default and dead objects and the other properties untouched");
        PhysicsSubstepCapOnGameThread();
        PhysicsSubstepCapOnGameThread();
        Check(PhysicsSubstepCapSets() == 1, "physcap: settled: no further write");
        SetPhysicsSubstepCap(0);
        PhysicsSubstepCapOnGameThread();
        Check(Get<float>(w.obj, World::kStep) == 0.02f && Get<int32_t>(w.obj, World::kSteps) == 6 && PhysicsSubstepCapSets() == 2,
              "physcap: live key 0 puts the game's 0.02 / 6 back");
        SetPhysicsSubstepCap(1);
        PhysicsSubstepCapOnGameThread();
        Check(Get<float>(w.obj, World::kStep) == 0.03f && PhysicsSubstepCapSets() == 3, "physcap: live key 1 again");
        w.Set(true, 0.05f, 4);  // someone else (a console command, a game change) after ours
        SetPhysicsSubstepCap(0);
        PhysicsSubstepCapOnGameThread();
        Check(Get<float>(w.obj, World::kStep) == 0.05f && PhysicsSubstepCapSets() == 3, "physcap: a later value of someone else is not put back");
        SetPhysicsSubstepCap(1);
        PhysicsSubstepCapOnGameThread();
        Check(Get<float>(w.obj, World::kStep) == 0.05f && PhysicsSubstepCapSets() == 3, "physcap: ... nor written over by key 1");
    }
    {
        World w;
        w.Set(true, 0.03f, 4);  // the player's two Engine.ini lines
        PhysicsSubstepCapTestSetup(w.array, fname, noIni);
        SetPhysicsSubstepCap(1);
        PhysicsSubstepCapOnGameThread();
        SetPhysicsSubstepCap(0);
        PhysicsSubstepCapOnGameThread();
        Check(Get<float>(w.obj, World::kStep) == 0.03f && Get<int32_t>(w.obj, World::kSteps) == 4 && PhysicsSubstepCapSets() == 0,
              "physcap: values other than the game's (Engine.ini) left alone, also when the key goes off");
    }
    {
        World w;
        w.Set(false, 0.02f, 6);
        PhysicsSubstepCapTestSetup(w.array, fname, noIni);
        SetPhysicsSubstepCap(1);
        PhysicsSubstepCapOnGameThread();
        Check(Get<float>(w.obj, World::kStep) == 0.02f && PhysicsSubstepCapSets() == 0, "physcap: substepping off: nothing written");
    }
    {
        World w;
        Check(PutFile(ini, "[Core.System]\r\nPaths=x\r\n\r\n[/Script/Engine.PhysicsSettings]\r\nMaxSubsteps=6\r\n"),
              "physcap: test Engine.ini written");
        PhysicsSubstepCapTestSetup(w.array, fname, ini);
        SetPhysicsSubstepCap(1);
        PhysicsSubstepCapOnGameThread();
        Check(Get<float>(w.obj, World::kStep) == 0.02f && Get<int32_t>(w.obj, World::kSteps) == 6 && PhysicsSubstepCapSets() == 0,
              "physcap: an Engine.ini line with the game's own value: left alone");
        DeleteFileW(ini);
    }
    {
        World w(1);  // MaxSubsteps registered as a FloatProperty: a layout we do not know
        PhysicsSubstepCapTestSetup(w.array, fname, noIni);
        SetPhysicsSubstepCap(1);
        PhysicsSubstepCapOnGameThread();
        Check(Get<float>(w.obj, World::kStep) == 0.02f && Get<int32_t>(w.obj, World::kSteps) == 6 && PhysicsSubstepCapSets() == 0,
              "physcap: a property of another type: nothing written");
    }
    {
        World w;
        Put<int32_t>(w.obj, 0x08, 0x01);  // no default object in the list
        PhysicsSubstepCapTestSetup(w.array, fname, noIni);
        PhysicsSubstepCapOnGameThread();
        Check(PhysicsSubstepCapSets() == 0, "physcap: the game-thread call does nothing before the monitor thread has looked");
        SetPhysicsSubstepCap(1);
        for (int i = 0; i < 300; ++i) PhysicsSubstepCapOnGameThread();
        Check(Get<float>(w.obj, World::kStep) == 0.02f && PhysicsSubstepCapSets() == 0, "physcap: no default object: nothing written, gives up");
    }
    // the Engine.ini line reader
    Check(PhysicsSubstepCapIniLines("[/Script/Engine.PhysicsSettings]\nMaxSubstepDeltaTime=0.03\n"), "physcap: ini: MaxSubstepDeltaTime line");
    Check(PhysicsSubstepCapIniLines("[/script/engine.physicssettings]\r\n  maxsubsteps = 4\r\n"), "physcap: ini: any case, spaces");
    Check(!PhysicsSubstepCapIniLines("[/Script/Engine.PhysicsSettings]\n;MaxSubsteps=4\nMaxSubstepsX=1\nbSubstepping=True\n"),
          "physcap: ini: comments, other keys");
    Check(!PhysicsSubstepCapIniLines("[SystemSettings]\nMaxSubsteps=4\n[/Script/Engine.PhysicsSettings]\n"),
          "physcap: ini: the keys in another section");
    Check(!PhysicsSubstepCapIniLines(""), "physcap: ini: empty");
    Check(PhysicsSubstepCapIniLines("\xEF\xBB\xBF[/Script/Engine.PhysicsSettings]\nMaxSubsteps=6\n"),
          "physcap: ini: a UTF-8 byte order mark before the section");
}
