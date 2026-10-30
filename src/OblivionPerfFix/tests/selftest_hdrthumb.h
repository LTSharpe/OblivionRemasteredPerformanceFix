// HdrSaveThumbnail (trade-off) self-test, included by selftest.cpp: the save's screenshot callback's copy call
// on the mapped exe, SaveThumbnailAsync's view of it (the game's copy behind the stub), and the wrapper with a fake copy
// and fake console variables (HDR10 converts the holder, SDR / scRGB / off leave the copy as it is).
#pragma once

#include <vector>

namespace hthumb {

struct Arr { uint32_t* data; int32_t num, max; };
inline std::vector<uint32_t> g_copied;
inline int g_copies = 0;
inline void* FakeCopy(void* dst, const void* src)
{
    const auto* s = static_cast<const Arr*>(src);
    auto* d = static_cast<Arr*>(dst);
    g_copied.assign(s->data, s->data + s->num);
    d->data = g_copied.data();
    d->num = d->max = s->num;
    ++g_copies;
    return dst;
}

inline void Run(const opf::GameModule& game)
{
    using namespace opf;
    Check(ApplyHdrSaveThumbnail(true), "hdr save thumbnail: the save's screenshot callback found, its copy call redirected");
    uint8_t *call = nullptr, *orig = nullptr, *stub = nullptr;
    void* hook = nullptr;
    if (!Installed(HdrSaveThumbnailInfo(&call, &orig, &stub, &hook), "HdrSaveThumbnail (save thumbnails in HDR)")) return;
    Check(call == game.base + 0x48AF8DA, "hdr save thumbnail: the callback (exe+0x48AF8A0) copies the pixels at exe+0x48AF8DA");
    Check(game.text.Contains(orig), "hdr save thumbnail: the game's TArray copy is in the game's code");
    int32_t rel = 0;
    memcpy(&rel, call + 1, 4);
    Check(call[0] == 0xE8 && call + 5 + rel == stub, "hdr save thumbnail: the copy call goes to the stub");
    void* target = nullptr;
    memcpy(&target, stub + 6, sizeof target);
    Check(stub[0] == 0xFF && stub[1] == 0x25 && target == hook, "hdr save thumbnail: the stub jumps to the wrapper");
    Check(HdrSaveThumbnailResolve(stub) == orig && HdrSaveThumbnailResolve(call) == call,
          "hdr save thumbnail: SaveThumbnailAsync sees the game's copy behind the stub");

    // the wrapper: the copy done, then the holder converted in HDR10 only (the source never touched)
    uint8_t *mcall = nullptr, *resize = nullptr, *mstub = nullptr;
    void* mhook = nullptr;
    void** manager = nullptr;
    const bool menuInfo = HdrMenuBackgroundInfo(&mcall, &resize, &mstub, &mhook, &manager);
    int32_t hdr = 1, device = 4;
    float ui = 250.f;
    HdrMenuBackgroundTestSetup(resize, &hdr, &device, &ui);
    HdrSaveThumbnailTestSetup(reinterpret_cast<void*>(&FakeCopy));
    uint8_t t[256];
    HdrMenuBackgroundTable(ui, t);
    const uint32_t in = 0xFF000000u | 0xBC << 16 | 0x70 << 8 | 0x80;  // R 188, G 112, B 128
    const uint32_t want = 0xFF000000u | static_cast<uint32_t>(t[0xBC]) << 16 | static_cast<uint32_t>(t[0x70]) << 8 | t[0x80];
    std::vector<uint32_t> source;
    using Fn = void* (*)(void*, const void*);
    auto runOnce = [&]() {
        source.assign(4, in);
        Arr src{source.data(), 4, 4}, dst{};
        g_copies = 0;
        void* const r = reinterpret_cast<Fn>(hook)(&dst, &src);
        const bool sourceKept = source[3] == in;
        return (r == &dst && g_copies == 1 && sourceKept && dst.num == 4) ? g_copied[3] : 0u;
    };
    SetHdrSaveThumbnail(false);
    Check(runOnce() == in, "hdr save thumbnail: off -> the game's copy, bytes unchanged");
    SetHdrSaveThumbnail(true);
    Check(runOnce() == want, "hdr save thumbnail: on, HDR10 (device 4) -> the holder converted per channel, alpha kept, source kept");
    device = 3;
    Check(runOnce() == want, "hdr save thumbnail: on, HDR10 (device 3) -> converted");
    hdr = 0;
    Check(runOnce() == in, "hdr save thumbnail: on, SDR -> the game's copy");
    hdr = 1;
    device = 5;
    Check(runOnce() == in, "hdr save thumbnail: on, scRGB output -> the game's copy");
    SetHdrSaveThumbnail(false);
    HdrSaveThumbnailTestSetup(orig);
    HdrMenuBackgroundTestSetup(menuInfo ? resize : nullptr, nullptr, nullptr, nullptr);
}

}  // namespace hthumb
