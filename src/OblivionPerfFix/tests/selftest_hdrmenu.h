// HdrMenuRows self-test, included by selftest.cpp: the engine helpers it calls are found where build
// 19115871 has them (the rows themselves are runtime data: checked live).
#pragma once

namespace hdrm {

struct Str { wchar_t* data; int32_t num, max; };
inline const uint8_t* FakeLookup(void*, const void* cvar)
{
    static const uint8_t fsr = 2;
    const auto* s = static_cast<const Str*>(cvar);
    return s && s->data && wcscmp(s->data, L"Altar.FSR3.Quality") == 0 ? &fsr : nullptr;
}

inline void Run(const opf::GameModule& game)
{
    using namespace opf;
    HdrMenuInfoOut h{};
    if (!Installed(FindHdrMenu(&h), "HdrMenuRows (HDR menu rows)")) return;
    Check(h.fromString == game.base + 0xE56680, "hdr menu: FText::FromString at exe+0xE56680");
    Check(h.fnameCtor == game.base + 0xF3B8B0, "hdr menu: FName(const TCHAR*, EFindName) at exe+0xF3B8B0");
    Check(h.malloc == game.base + 0xE32F40, "hdr menu: FMemory::Malloc at exe+0xE32F40");
    Check(h.free != nullptr && game.text.Contains(h.free), "hdr menu: FMemory::Free found");
    Check(h.gmalloc == game.base + 0x9032560, "hdr menu: GMalloc at exe+0x9032560");
    Check(h.objectArray == game.base + 0x9112A60, "hdr menu: GUObjectArray at exe+0x9112A60");
    Check(FindObjectArrayFromDtor() == h.objectArray, "hdr menu: GUObjectArray as ~UObjectBase gives it");
    Check(h.setter == game.base + 0x48F7F10, "hdr menu: Altar's command setter at exe+0x48F7F10");
    Check(h.addOrUpdate == game.base + 0x4757FA0, "hdr menu: AddOrUpdateSettingsToSave at exe+0x4757FA0 (by its registered name)");
    Check(h.saveWriter == game.base + 0x476B290, "hdr menu: the settings save's writer at exe+0x476B290");
    Check(h.saveSlot == game.base + 0x92CFEE0, "hdr menu: its slot name FString at exe+0x92CFEE0");
    Check(h.spawnObject == game.base + 0x31C1860, "hdr menu: UGameplayStatics::SpawnObject at exe+0x31C1860 (by its registered name)");

    // DlssRayReconstruction's row lock: the settings widgets' upscaler-row lookup answered DLSS for its variable only
    uint8_t *call = nullptr, *orig = nullptr, *stub = nullptr;
    void* hook = nullptr;
    if (!Installed(RrLockInfo(&call, &orig, &stub, &hook), "DlssRayReconstruction (the row greyed with the DLSS rows)")) return;
    Check(call == game.base + 0x495BF8C, "rr row: the upscaler lock (exe+0x495BF70) looks its row up at exe+0x495BF8C");
    Check(orig == game.base + 0x495AFD0, "rr row: the command -> upscaler table lookup at exe+0x495AFD0");
    int32_t rel = 0;
    memcpy(&rel, call + 1, 4);
    Check(call[0] == 0xE8 && call + 5 + rel == stub, "rr row: the lock's call goes to the stub");
    RrLockTestSetup(reinterpret_cast<void*>(&FakeLookup));
    using Fn = const uint8_t* (*)(void*, const void*);
    auto ask = [&](const wchar_t* name) {
        const Str s{const_cast<wchar_t*>(name), static_cast<int32_t>(wcslen(name)) + 1, static_cast<int32_t>(wcslen(name)) + 1};
        const uint8_t* const r = reinterpret_cast<Fn>(hook)(nullptr, &s);
        return r ? static_cast<int>(*r) : -1;
    };
    Check(ask(L"Altar.FSR3.Quality") == 2, "rr row: the game's own entries pass through (FSR)");
    Check(ask(L"r.NGX.DLSS.DenoiserMode") == 1, "rr row: r.NGX.DLSS.DenoiserMode answered DLSS");
    Check(ask(L"r.NGX.DLSS.DenoiserModeX") == -1 && ask(L"r.NGX.DLSS.Denoiser") == -1 && ask(L"Altar.Other") == -1,
          "rr row: any other unknown variable stays unknown");
    RrLockTestSetup(orig);

    // HudAspectRow: ProcessEvent, and the padding for a centred box (5120x2160 at scale 2 = 2560x1080 Slate units)
    Check(FindProcessEvent() == game.base + 0x10F3EA0, "hud aspect: UObject::ProcessEvent at exe+0x10F3EA0 (by its prologue)");
    Check(HudAspectIntRegistrationFound(), "hud aspect: the console manager's int registration site (r.VT.MaxUploadsPerFrame.Streaming's) found once");
    auto approx = [](float a, float b) { return a > b - 0.01f && a < b + 0.01f; };
    Check(approx(HudPadding(5120, 2160, 2, 16.f / 9.f), 320.f) && approx(HudPadding(5120, 2160, 2, 4.f / 3.f), 560.f) &&
              approx(HudPadding(5120, 2160, 2, 21.f / 9.f), 20.f),
          "hud aspect: 5120x2160 at scale 2: 16:9 pads 320, 4:3 560, 21:9 20 per side");
    Check(HudPadding(5120, 2160, 2, 32.f / 9.f) == 0.f && HudPadding(5120, 2160, 2, 0.f) == 0.f && HudPadding(3840, 2160, 2, 16.f / 9.f) == 0.f,
          "hud aspect: a box as wide as the screen or wider, and Wide, pad nothing");
    Check(HudPadding(0, 2160, 2, 16.f / 9.f) < 0 && HudPadding(5120, 2160, 0, 16.f / 9.f) < 0, "hud aspect: no viewport gives -1");
}

}  // namespace hdrm
