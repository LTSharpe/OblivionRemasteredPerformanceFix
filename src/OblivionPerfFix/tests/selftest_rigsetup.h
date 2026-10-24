// RigVmHashMemo / RigLogicMapCache self-test, included by selftest.cpp: the sites on the mapped exe, then both caches
// with fakes. VM hash: a fake ComputeVMHash over every input the real one reads; the memo must return its value for
// equal inputs (also a copy of the VM at another address), call it again for any change (one byte of byte code, a name,
// an entry, a branch, an external variable's type, the alignment flag, a memory class), and never mismatch in verify
// mode. Maps: fake DNA readers (IDNAReader getters + an Unwrap of the game's code shape + dna::Reader string getters),
// contexts, hierarchies and reference map functions written from the UE 5.3 source (the same TArray operations as the
// game's); the same content in another reader must replay to the reference's arrays (values, counts, capacities),
// early returns included; every change the stock code reads must miss that map only, changes it does not read must
// hit; reader classes whose dna strings disagree with the FString getters, or whose Unwrap has another shape, are not
// used; verify mode must count no mismatch.
#pragma once
#include <map>
#include <random>
#include <string>

namespace rsu {

// ---------------- fakes: game helpers
uint8_t g_genClass[0x40];   // the memory storage generator class: chain depth at +0x38, chain at +0x30
uint8_t* g_genChain[4];
uint8_t* FakeStaticClass() { return g_genClass; }
uint32_t FakeMemoryHash(uint8_t* cls) { return *reinterpret_cast<uint32_t*>(cls + 0x238); }
int g_vmOrigCalls = 0;
uint64_t MixBytes(uint64_t h, const void* p, size_t n)
{
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}
struct FakeClass {  // a UClass: +0x10 its metaclass, +0x238 CachedMemoryHash
    uint8_t b[0x240] = {};
};
struct FakeMeta {  // the metaclass: +0x30 chain, +0x38 depth
    uint8_t b[0x40] = {};
};
uint32_t MemPartRef(uint8_t* storage)
{
    uint8_t* cls = *reinterpret_cast<uint8_t**>(storage + 0x10);
    if (!cls) return 0;
    uint8_t* meta = *reinterpret_cast<uint8_t**>(cls + 0x10);
    const int32_t d = *reinterpret_cast<int32_t*>(g_genClass + 0x38);
    if (d > *reinterpret_cast<int32_t*>(meta + 0x38)) return 0;
    if ((*reinterpret_cast<uint8_t***>(meta + 0x30))[d] != g_genClass + 0x30) return 0;
    return *reinterpret_cast<uint32_t*>(cls + 0x238);
}
// The fake ComputeVMHash: a hash of every field the real one reads (and of the code's address modulo 16 when aligned).
uint32_t FakeVmHash(const uint8_t* vm)
{
    ++g_vmOrigCalls;
    uint64_t h = 1469598103934665603ull;
    const uint8_t* names = *reinterpret_cast<uint8_t* const*>(vm + 0x130);
    h = MixBytes(h, *reinterpret_cast<uint8_t* const*>(names), static_cast<size_t>(*reinterpret_cast<const int32_t*>(names + 8)) * 8);
    const uint8_t* bc = *reinterpret_cast<uint8_t* const*>(vm + 0x100);
    const uint8_t* code = *reinterpret_cast<uint8_t* const*>(bc);
    h = MixBytes(h, code, static_cast<size_t>(*reinterpret_cast<const int32_t*>(bc + 8)));
    if (bc[0x98]) h ^= (reinterpret_cast<uintptr_t>(code) & 15) * 0x9E37;
    h = MixBytes(h, *reinterpret_cast<uint8_t* const*>(bc + 0x18), static_cast<size_t>(*reinterpret_cast<const int32_t*>(bc + 0x20)) * 0xC);
    h = MixBytes(h, *reinterpret_cast<uint8_t* const*>(bc + 0x28), static_cast<size_t>(*reinterpret_cast<const int32_t*>(bc + 0x30)) * 0x18);
    const uint8_t* ext = *reinterpret_cast<uint8_t* const*>(vm + 0x1D8);
    for (int32_t i = 0; i < *reinterpret_cast<const int32_t*>(vm + 0x1E0); ++i) {
        h = MixBytes(h, ext + i * 0x28, 8);
        h = MixBytes(h, ext + i * 0x28 + 0x10, 8);
    }
    uint8_t* lit = *reinterpret_cast<uint8_t* const*>(vm + 0x30);
    uint8_t* work = *reinterpret_cast<uint8_t* const*>(vm + 0x28);
    if (lit) h = h * 31 + MemPartRef(lit) + 7;
    if (work) h = h * 37 + MemPartRef(work) + 11;
    return static_cast<uint32_t>(h ^ (h >> 32));
}

struct FakeVm {
    std::vector<uint64_t> names;
    std::vector<uint8_t> code;  // over-allocated to place the byte code at a chosen alignment
    size_t codeOff = 0;
    int32_t codeNum = 0;
    std::vector<uint8_t> entries, branches, ext;
    uint8_t bc[0xA0] = {};
    uint8_t namesArr[16] = {};
    uint8_t lit[0x20] = {}, work[0x20] = {};
    uint8_t vm[0x200] = {};
    bool hasLit = true, hasWork = true;
    void Build()
    {
        uint64_t* nd = names.data();
        memcpy(namesArr, &nd, 8);
        const int32_t nn = static_cast<int32_t>(names.size());
        memcpy(namesArr + 8, &nn, 4);
        uint8_t* p = namesArr;
        memcpy(vm + 0x130, &p, 8);
        uint8_t* c = code.data() + codeOff;
        memcpy(bc, &c, 8);
        memcpy(bc + 8, &codeNum, 4);
        uint8_t* e = entries.data();
        const int32_t en = static_cast<int32_t>(entries.size() / 0xC);
        memcpy(bc + 0x18, &e, 8);
        memcpy(bc + 0x20, &en, 4);
        uint8_t* b = branches.data();
        const int32_t bn = static_cast<int32_t>(branches.size() / 0x18);
        memcpy(bc + 0x28, &b, 8);
        memcpy(bc + 0x30, &bn, 4);
        uint8_t* bp = bc;
        memcpy(vm + 0x100, &bp, 8);
        uint8_t* x = ext.data();
        const int32_t xn = static_cast<int32_t>(ext.size() / 0x28);
        memcpy(vm + 0x1D8, &x, 8);
        memcpy(vm + 0x1E0, &xn, 4);
        uint8_t* l = hasLit ? lit : nullptr;
        uint8_t* w = hasWork ? work : nullptr;
        memcpy(vm + 0x30, &l, 8);
        memcpy(vm + 0x28, &w, 8);
    }
};

// ---------------- fakes: TArrays with the game's growth functions
struct I32Arr {
    int32_t* data;
    int32_t num, max;
};
struct NestArr {
    I32Arr* data;
    int32_t num, max;
};
void FakeResizeTo(void* a, int32_t n)
{
    I32Arr* x = static_cast<I32Arr*>(a);
    x->data = static_cast<int32_t*>(realloc(x->data, static_cast<size_t>(n > 0 ? n : 1) * 4));
    x->max = n;
}
void FakeGrow32(void* a, int32_t)
{
    I32Arr* x = static_cast<I32Arr*>(a);
    const int32_t m = x->num + x->num / 2 + 4;
    x->data = static_cast<int32_t*>(realloc(x->data, static_cast<size_t>(m) * 4));
    x->max = m;
}
void FakeGrow16(void* a, int32_t)
{
    NestArr* x = static_cast<NestArr*>(a);
    const int32_t m = x->num + 3;
    x->data = static_cast<I32Arr*>(realloc(x->data, static_cast<size_t>(m) * 16));
    x->max = m;
}
void FakeEmpty16(void* a, int32_t)
{
    NestArr* x = static_cast<NestArr*>(a);
    free(x->data);
    x->data = nullptr;
    x->max = 0;
}
void FakeFree(void* p) { free(p); }

// FNames: a fake pool (string -> index), FName = {index, number 0}
std::map<std::wstring, uint32_t> g_pool;
uint64_t* FakeFNameCtor(uint64_t* out, const wchar_t* s, int)
{
    auto it = g_pool.find(s);
    if (it == g_pool.end()) it = g_pool.emplace(s, static_cast<uint32_t>(g_pool.size() + 1)).first;
    *out = it->second;
    return out;
}
uint32_t FakeNameHash(uint32_t i) { return i * 2654435761u; }

// ---------------- fake hierarchy (IndexLookup as the game's TMap)
struct FakeHier {
    uint8_t b[0x110] = {};
    std::vector<uint8_t> elems;
    std::vector<int32_t> hash;
    void Build(const std::vector<std::pair<uint8_t, uint64_t>>& keys)
    {
        elems.assign(keys.size() * 0x18 + 0x18, 0);
        hash.assign(64, -1);
        for (size_t i = 0; i < keys.size(); ++i) {
            uint8_t* e = elems.data() + i * 0x18;
            e[0] = keys[i].first;
            memcpy(e + 4, &keys[i].second, 8);
            const int32_t v = static_cast<int32_t>(i * 3 + 1);
            memcpy(e + 0xC, &v, 4);
            const uint32_t hh = FakeNameHash(static_cast<uint32_t>(keys[i].second));
            const uint32_t bucket = 63u & ((hh + static_cast<uint32_t>(keys[i].second >> 32)) * 10u + keys[i].first);
            memcpy(e + 0x10, &hash[bucket], 4);
            hash[bucket] = static_cast<int32_t>(i);
        }
        uint8_t* ep = elems.data();
        memcpy(b + 0xB8, &ep, 8);
        const int32_t n = static_cast<int32_t>(keys.size()), zero = 0, hs = 64;
        memcpy(b + 0xC0, &n, 4);
        memcpy(b + 0xEC, &zero, 4);
        int32_t* hp = hash.data();
        memcpy(b + 0xF8, &hp, 8);
        memcpy(b + 0x100, &hs, 4);
    }
};
int32_t RefFind(const FakeHier& h, uint8_t type, uint64_t name)
{
    const size_t n = (h.elems.size() - 0x18) / 0x18;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* e = h.elems.data() + i * 0x18;
        if (e[0] == type && memcmp(e + 4, &name, 8) == 0) return *reinterpret_cast<const int32_t*>(e + 0xC);
    }
    return -1;
}

// ---------------- fake DNA reader / context / controller
// IDNAReader side: context[0] = FakeReader*, reader+8 = vbtable (+4: 8) -> the definition interface at reader+0x10, whose
// vtable has the stock getters at the game's slots. dna side: the interface's Unwrap (machine code of the game's shape:
// sub rcx,8; jmp -> mov rax,[rcx+18h]; ret) returns a FakeDnaReader whose definition sub-object (again via vbptr at +8)
// has the dna::DefinitionReader string getters (StringView of narrow strings).
struct FakeDna {
    std::vector<std::wstring> joints, controls;            // controls: "obj.attr" (one without '.' = early return)
    std::vector<std::wstring> meshes, blendShapes, animMaps;
    std::vector<std::vector<std::pair<uint16_t, uint16_t>>> morphPerLod;  // (mesh, blend shape channel)
    std::vector<std::vector<uint16_t>> animPerLod;
    // derived by Finalize(): narrow copies, the global mapping list and the per-LOD mapping indices
    std::vector<std::string> nJoints, nControls, nMeshes, nBs, nAnim;
    std::vector<std::pair<uint16_t, uint16_t>> mappings;
    std::vector<std::vector<uint16_t>> morphIdx;
    void Finalize()
    {
        auto narrow = [](const std::vector<std::wstring>& w, std::vector<std::string>& n) {
            n.clear();
            for (auto& s : w) {
                std::string x;
                for (wchar_t ch : s) x.push_back(static_cast<char>(ch));
                n.push_back(x);
            }
        };
        narrow(joints, nJoints);
        narrow(controls, nControls);
        narrow(meshes, nMeshes);
        narrow(blendShapes, nBs);
        narrow(animMaps, nAnim);
        mappings.clear();
        morphIdx.assign(morphPerLod.size(), {});
        for (size_t l = 0; l < morphPerLod.size(); ++l)
            for (auto& m : morphPerLod[l]) {
                morphIdx[l].push_back(static_cast<uint16_t>(mappings.size()));
                mappings.push_back(m);
            }
    }
};
struct FakeDnaReader;
struct FakeIface {
    void** vtbl;
    FakeDna* dna;
    FakeDnaReader* dnaReader;  // +0x10: what Unwrap returns
};
struct FakeReader {
    uint8_t pad[8];
    uint8_t* vb;      // +8: vbtable (+4 = offset of the interface from reader+8)
    FakeIface iface;  // at reader + 0x10
};
struct FakeDef {
    void** vtbl;
    FakeDna* dna;
};
struct FakeDnaReader {
    void* vfptr;
    uint8_t* vb;  // +8: vbtable -> definition sub-object at +0x10
    FakeDef def;
};
struct View {
    const uint16_t* data;
    int32_t num, pad;
};
struct FStr {
    wchar_t* data;
    int32_t num, max;
};
struct StrView {
    const char* data;
    uint64_t size;
};
FakeDna* DnaOf(void* iface) { return static_cast<FakeIface*>(iface)->dna; }
uint16_t VLodCount(void* i) { return static_cast<uint16_t>(DnaOf(i)->morphPerLod.size()); }
uint16_t VRawControlCount(void* i) { return static_cast<uint16_t>(DnaOf(i)->controls.size()); }
uint16_t VJointCount(void* i) { return static_cast<uint16_t>(DnaOf(i)->joints.size()); }
View* VAnimMapIndices(void* i, View* out, uint32_t lod)
{
    auto& v = DnaOf(i)->animPerLod[lod];
    out->data = v.data();
    out->num = static_cast<int32_t>(v.size());
    return out;
}
View* VBlendMapIndices(void* i, View* out, uint32_t lod)
{
    auto& v = DnaOf(i)->morphIdx[lod];
    out->data = v.data();
    out->num = static_cast<int32_t>(v.size());
    return out;
}
int32_t* VMapping(void* i, int32_t* out, uint32_t idx)  // FMeshBlendShapeChannelMapping: int32 mesh, int32 channel
{
    auto& m = DnaOf(i)->mappings[idx & 0xFFFF];
    out[0] = m.first;
    out[1] = m.second;
    return out;
}
FStr* MakeF(FStr* out, const std::wstring& s)  // ANSI_TO_TCHAR -> FString (an empty string allocates nothing)
{
    if (s.empty()) {
        out->data = nullptr;
        out->num = out->max = 0;
        return out;
    }
    const int32_t n = static_cast<int32_t>(s.size()) + 1;
    out->data = static_cast<wchar_t*>(malloc(static_cast<size_t>(n) * 2));
    memcpy(out->data, s.c_str(), static_cast<size_t>(n) * 2);
    out->num = out->max = n;
    return out;
}
FStr* VRawName(void* i, FStr* o, uint32_t x) { return MakeF(o, DnaOf(i)->controls[x & 0xFFFF]); }
FStr* VJointName(void* i, FStr* o, uint32_t x) { return MakeF(o, DnaOf(i)->joints[x & 0xFFFF]); }
FStr* VBsName(void* i, FStr* o, uint32_t x) { return MakeF(o, DnaOf(i)->blendShapes[x & 0xFFFF]); }
FStr* VAnimName(void* i, FStr* o, uint32_t x) { return MakeF(o, DnaOf(i)->animMaps[x & 0xFFFF]); }
FStr* VMeshName(void* i, FStr* o, uint32_t x) { return MakeF(o, DnaOf(i)->meshes[x & 0xFFFF]); }
volatile int g_cppUnwraps = 0;
FakeDnaReader* CppUnwrap(void* i)  // not the game's code shape (a plain getter would be: mov rax,[rcx+10h]; ret)
{
    g_cppUnwraps = g_cppUnwraps + 1;
    return static_cast<FakeIface*>(i)->dnaReader;
}
FakeDna* DefDna(void* def) { return static_cast<FakeDef*>(def)->dna; }
StrView* SV(StrView* o, const std::string& s)
{
    o->data = s.c_str();
    o->size = s.size();
    return o;
}
StrView* DRaw(void* d, StrView* o, uint32_t x) { return SV(o, DefDna(d)->nControls[x & 0xFFFF]); }
StrView* DJoint(void* d, StrView* o, uint32_t x) { return SV(o, DefDna(d)->nJoints[x & 0xFFFF]); }
StrView* DBs(void* d, StrView* o, uint32_t x) { return SV(o, DefDna(d)->nBs[x & 0xFFFF]); }
StrView* DAnim(void* d, StrView* o, uint32_t x) { return SV(o, DefDna(d)->nAnim[x & 0xFFFF]); }
StrView* DMesh(void* d, StrView* o, uint32_t x) { return SV(o, DefDna(d)->nMeshes[x & 0xFFFF]); }
const std::string g_lie = "not_the_same";
StrView* DJointLie(void*, StrView* o, uint32_t) { return SV(o, g_lie); }  // a reader class whose dna slots are not what we assume
void* g_vtbl[0x160 / 8] = {};     // IDNAReader definition interface
void* g_vtblCpp[0x160 / 8] = {};  // the same with a C++ Unwrap (refused: unknown code shape)
void* g_defVtbl[0x130 / 8] = {};
void* g_defVtblLiar[0x130 / 8] = {};
uint8_t* g_unwrapCode = nullptr;
uint8_t g_vbtable[8] = {0, 0, 0, 0, 8, 0, 0, 0};  // the sub-object at +8+8
void SetupVtables()
{
    for (auto* t : {g_vtbl, g_vtblCpp}) {
        for (int k = 0; k < 0x160 / 8; ++k) t[k] = nullptr;
        t[0x78 / 8] = reinterpret_cast<void*>(&VLodCount);
        t[0xA8 / 8] = reinterpret_cast<void*>(&VRawControlCount);
        t[0xB0 / 8] = reinterpret_cast<void*>(&VRawName);
        t[0xB8 / 8] = reinterpret_cast<void*>(&VJointCount);
        t[0xC0 / 8] = reinterpret_cast<void*>(&VJointName);
        t[0xE8 / 8] = reinterpret_cast<void*>(&VBsName);
        t[0x108 / 8] = reinterpret_cast<void*>(&VAnimName);
        t[0x118 / 8] = reinterpret_cast<void*>(&VAnimMapIndices);
        t[0x128 / 8] = reinterpret_cast<void*>(&VMeshName);
        t[0x148 / 8] = reinterpret_cast<void*>(&VMapping);
        t[0x150 / 8] = reinterpret_cast<void*>(&VBlendMapIndices);
    }
    if (!g_unwrapCode) {
        g_unwrapCode = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        const uint8_t code[] = {0x48, 0x83, 0xE9, 0x08, 0xE9, 0x07, 0x00, 0x00, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
                                0x48, 0x8B, 0x41, 0x18, 0xC3};  // sub rcx,8; jmp +7 -> mov rax,[rcx+18h]; ret
        memcpy(g_unwrapCode, code, sizeof code);
        FlushInstructionCache(GetCurrentProcess(), g_unwrapCode, 4096);
    }
    g_vtbl[1] = g_unwrapCode;
    g_vtblCpp[1] = reinterpret_cast<void*>(&CppUnwrap);
    for (auto* t : {g_defVtbl, g_defVtblLiar}) {
        for (int k = 0; k < 0x130 / 8; ++k) t[k] = nullptr;
        t[0xA0 / 8] = reinterpret_cast<void*>(&DRaw);
        t[0xB0 / 8] = reinterpret_cast<void*>(&DJoint);
        t[0xD8 / 8] = reinterpret_cast<void*>(&DBs);
        t[0xF8 / 8] = reinterpret_cast<void*>(&DAnim);
        t[0x118 / 8] = reinterpret_cast<void*>(&DMesh);
    }
    g_defVtblLiar[0xB0 / 8] = reinterpret_cast<void*>(&DJointLie);
}
struct FakeRig {
    FakeReader reader;
    FakeDnaReader dnaReader;
    uint8_t context[0x40] = {};
    uint8_t controller[0x20] = {};
    uint8_t data[0xA0] = {};
    void Init(FakeDna* dna, void** ivt = g_vtbl, void** dvt = g_defVtbl)
    {
        reader.vb = g_vbtable;
        reader.iface.vtbl = ivt;
        reader.iface.dna = dna;
        reader.iface.dnaReader = &dnaReader;
        dnaReader.vfptr = nullptr;
        dnaReader.vb = g_vbtable;
        dnaReader.def.vtbl = dvt;
        dnaReader.def.dna = dna;
        FakeReader* rp = &reader;
        memcpy(context, &rp, 8);
        uint8_t* c = context;
        memcpy(data + 8, &c, 8);
        uint8_t* ct = controller;
        memcpy(data + 0x10, &ct, 8);
        const int32_t one = 1;
        memcpy(controller + 8, &one, 4);
        memcpy(controller + 0xC, &one, 4);
    }
    I32Arr* Arr(size_t off) { return reinterpret_cast<I32Arr*>(data + off); }
    NestArr* Nest(size_t off) { return reinterpret_cast<NestArr*>(data + off); }
    void Free()
    {
        for (size_t off : {0x20, 0x40}) free(Arr(off)->data);
        for (size_t off : {0x50, 0x60, 0x70, 0x80}) {
            NestArr* n = Nest(off);
            for (int32_t i = 0; i < n->num; ++i) free(n->data[i].data);
            free(n->data);
        }
        memset(data + 0x20, 0, 0x80);
    }
};

// ---------------- reference map functions (UE 5.3 RigUnit_RigLogic.cpp, the game's array operations)
using NameHookFn = uint64_t* (*)(uint64_t*, const wchar_t*, int);
NameHookFn g_nameHook = nullptr;
FakeHier* g_refHier = nullptr;  // the reference functions look up in the hierarchy passed (the same as the hook's)
uint64_t Name(const std::wstring& s)
{
    uint64_t n = 0;
    g_nameHook(&n, s.c_str(), 1);
    return n;
}
int32_t FindIn(uint8_t* h, uint8_t type, uint64_t name)
{
    return RefFind(*reinterpret_cast<FakeHier*>(h), type, name);  // b is FakeHier's first member
}
void Add32(I32Arr* a, int32_t v)
{
    const int32_t i = a->num;
    a->num = i + 1;
    if (static_cast<uint32_t>(a->max) < static_cast<uint32_t>(i + 1)) FakeGrow32(a, i);
    a->data[i] = v;
}
void ResetNested(NestArr* a, uint32_t n)
{
    for (int32_t i = 0; i < a->num; ++i)
        if (a->data[i].data) free(a->data[i].data);
    a->num = 0;
    const int32_t old = a->num;
    a->num = old + static_cast<int32_t>(n);
    if (static_cast<uint32_t>(a->max - old) < n) FakeGrow16(a, old);
    memset(a->data + old, 0, static_cast<size_t>(n) * 16);
}
FakeDna* DnaOfData(uint8_t* d)
{
    uint8_t* ctx = *reinterpret_cast<uint8_t**>(d + 8);
    return (*reinterpret_cast<FakeReader**>(ctx))->iface.dna;
}
std::wstring CurveName(const std::wstring& s)  // ConstructCurveName(s, "<obj>_<attr>")
{
    const size_t dot = s.find(L'.');
    if (dot == std::wstring::npos) return L"";
    return s.substr(0, dot) + L"_" + s.substr(dot + 1);
}
void RefJoints(uint8_t* d, uint8_t* h)
{
    FakeDna* dna = DnaOfData(d);
    I32Arr* a = reinterpret_cast<I32Arr*>(d + 0x40);
    const int32_t n = static_cast<int32_t>(dna->joints.size());
    a->num = 0;
    if (n != a->max && a->max <= n) FakeResizeTo(a, n);
    for (auto& j : dna->joints) Add32(a, FindIn(h, 1, Name(j)));
}
void RefInput(uint8_t* d, uint8_t* h)
{
    FakeDna* dna = DnaOfData(d);
    I32Arr* a = reinterpret_cast<I32Arr*>(d + 0x20);
    const int32_t n = static_cast<int32_t>(dna->controls.size());
    a->num = 0;
    if (a->max < n) FakeResizeTo(a, n);
    for (auto& c : dna->controls) {
        const std::wstring s = CurveName(c);
        if (s.empty()) return;
        const uint64_t nm = Name(s);
        Add32(a, h ? FindIn(h, 8, nm) : -1);
    }
}
void RefMorph(uint8_t* d, uint8_t* h)
{
    FakeDna* dna = DnaOfData(d);
    NestArr* a = reinterpret_cast<NestArr*>(d + 0x50);
    NestArr* b = reinterpret_cast<NestArr*>(d + 0x60);
    const uint32_t L = static_cast<uint32_t>(dna->morphPerLod.size());
    ResetNested(a, L);
    ResetNested(b, L);
    for (uint32_t lod = 0; lod < L; ++lod) {
        const int32_t c = static_cast<int32_t>(dna->morphPerLod[lod].size());
        if (a->data[lod].max < c) FakeResizeTo(&a->data[lod], c);
        if (b->data[lod].max < c) FakeResizeTo(&b->data[lod], c);
        for (auto& m : dna->morphPerLod[lod]) {
            const uint64_t nm = Name(dna->meshes[m.first] + L"__" + dna->blendShapes[m.second]);
            Add32(&a->data[lod], FindIn(h, 8, nm));
            Add32(&b->data[lod], m.second);
        }
    }
}
void RefMask(uint8_t* d, uint8_t* h)
{
    FakeDna* dna = DnaOfData(d);
    NestArr* a = reinterpret_cast<NestArr*>(d + 0x70);
    NestArr* b = reinterpret_cast<NestArr*>(d + 0x80);
    const uint32_t L = static_cast<uint32_t>(dna->morphPerLod.size());
    ResetNested(a, L);
    ResetNested(b, L);
    for (uint32_t lod = 0; lod < L; ++lod) {
        const int32_t c = static_cast<int32_t>(dna->animPerLod[lod].size());
        if (a->data[lod].max < c) FakeResizeTo(&a->data[lod], c);
        if (b->data[lod].max < c) FakeResizeTo(&b->data[lod], c);
        for (uint16_t idx : dna->animPerLod[lod]) {
            const std::wstring s = CurveName(dna->animMaps[idx]);
            if (s.empty()) return;
            Add32(&a->data[lod], FindIn(h, 8, Name(s)));
            Add32(&b->data[lod], idx);
        }
    }
}

bool SameArr(const I32Arr* a, const I32Arr* b)
{
    return a->num == b->num && a->max == b->max && (a->num == 0 || memcmp(a->data, b->data, static_cast<size_t>(a->num) * 4) == 0);
}
bool SameNest(const NestArr* a, const NestArr* b)
{
    if (a->num != b->num || a->max != b->max) return false;
    for (int32_t i = 0; i < a->num; ++i)
        if (!SameArr(a->data + i, b->data + i)) return false;
    return true;
}
bool SameRig(FakeRig& x, FakeRig& y)
{
    return SameArr(x.Arr(0x20), y.Arr(0x20)) && SameArr(x.Arr(0x40), y.Arr(0x40)) && SameNest(x.Nest(0x50), y.Nest(0x50)) &&
           SameNest(x.Nest(0x60), y.Nest(0x60)) && SameNest(x.Nest(0x70), y.Nest(0x70)) && SameNest(x.Nest(0x80), y.Nest(0x80));
}

FakeDna MakeDna(std::mt19937& rng, bool earlyInput, bool earlyMask)
{
    FakeDna d;
    const int nj = 20 + static_cast<int>(rng() % 30);
    for (int i = 0; i < nj; ++i) d.joints.push_back(L"FACIAL_J" + std::to_wstring(rng() % 60));
    const int nc = 10 + static_cast<int>(rng() % 20);
    for (int i = 0; i < nc; ++i) d.controls.push_back(L"CTRL_" + std::to_wstring(i) + L".ty" + std::to_wstring(rng() % 3));
    if (earlyInput) d.controls[nc / 2] = L"broken";
    for (int i = 0; i < 3; ++i) d.meshes.push_back(L"head_lod" + std::to_wstring(i));
    for (int i = 0; i < 25; ++i) d.blendShapes.push_back(L"bs" + std::to_wstring(i));
    for (int i = 0; i < 15; ++i) d.animMaps.push_back(L"FRM_" + std::to_wstring(i) + L".map" + std::to_wstring(i % 4));
    if (earlyMask) d.animMaps[7] = L"nodot";
    const int lods = 1 + static_cast<int>(rng() % 4);
    for (int l = 0; l < lods; ++l) {
        std::vector<std::pair<uint16_t, uint16_t>> m;
        const int n = 5 + static_cast<int>(rng() % 20);
        for (int j = 0; j < n; ++j) m.emplace_back(static_cast<uint16_t>(rng() % 3), static_cast<uint16_t>(rng() % 25));
        d.morphPerLod.push_back(m);
        std::vector<uint16_t> a;
        const int na = static_cast<int>(rng() % 12);
        for (int j = 0; j < na; ++j) a.push_back(static_cast<uint16_t>(rng() % 15));
        d.animPerLod.push_back(a);
    }
    d.Finalize();
    return d;
}
std::vector<std::pair<uint8_t, uint64_t>> MakeKeys(std::mt19937& rng)
{
    // bones and curves named like the DNA's, about half present, in a random order
    std::vector<std::pair<uint8_t, uint64_t>> keys;
    uint64_t n = 0;
    for (int i = 0; i < 60; ++i)
        if (rng() % 2) keys.emplace_back(uint8_t{1}, *FakeFNameCtor(&n, (L"FACIAL_J" + std::to_wstring(i)).c_str(), 1));
    for (int i = 0; i < 30; ++i)
        for (int t = 0; t < 3; ++t)
            if (rng() % 2) keys.emplace_back(uint8_t{8}, *FakeFNameCtor(&n, (L"CTRL_" + std::to_wstring(i) + L"_ty" + std::to_wstring(t)).c_str(), 1));
    for (int m = 0; m < 3; ++m)
        for (int i = 0; i < 25; ++i)
            if (rng() % 2)
                keys.emplace_back(uint8_t{8}, *FakeFNameCtor(&n, (L"head_lod" + std::to_wstring(m) + L"__bs" + std::to_wstring(i)).c_str(), 1));
    for (int i = 0; i < 15; ++i)
        if (rng() % 2) keys.emplace_back(uint8_t{8}, *FakeFNameCtor(&n, (L"FRM_" + std::to_wstring(i) + L"_map" + std::to_wstring(i % 4)).c_str(), 1));
    std::shuffle(keys.begin(), keys.end(), rng);
    return keys;
}

inline void RigSetupSelfTest(const opf::GameModule& game)
{
    using namespace opf;
    Check(InstallRigSetup(true), "rigsetup: ComputeVMHash and InitializeRigLogic's map calls found, installed");
    {
        const uint8_t* const b = game.base;
        Check(RigSetupTestSites(0) == b + 0x49A95D0 && b[0x49A95D0] == 0xE9, "rigsetup: ComputeVMHash (exe+0x49A95D0) jumps to the memo");
        bool calls = true;
        for (uint32_t site : {0x4E443C6u, 0x4E443D1u, 0x4E443E7u, 0x4E443F2u, 0x4E44E7Eu, 0x4E44C8Bu, 0x4E45D95u, 0x4E4555Cu}) {
            const int64_t t = static_cast<int64_t>(site) + 5 + *reinterpret_cast<const int32_t*>(b + site + 1);
            calls &= b[site] == 0xE8 && (t < 0 || t > 0x9DE5000);  // now outside the exe: our stubs
        }
        const int64_t neural = 0x4E443DC + 5 + *reinterpret_cast<const int32_t*>(b + 0x4E443DD);
        // FDNAReader<BinaryStreamReader>'s Unwrap through the definition interface's vtable (exe+0x7E7E740 slot 1): the
        // vtordisp adjustor movsxd rax,[rcx-4]; sub rcx,rax; jmp -> mov rax,[rcx-10h]; ret
        Check(RigSetupTestUnwrapShape(b + 0x4E475B4) && !RigSetupTestUnwrapShape(b + 0x4E44DD0),
              "rigsetup: the game's IDNAReader::Unwrap (exe+0x4E475B4) has the accepted code shape, a map function does not");
        Check(calls && neural == 0x4E45F30,
              "rigsetup: InitializeRigLogic's joint/control/morph/mask calls and the four maps' FName calls redirected, the neural map's not");
    }
    // ---- RigVmHashMemo with fakes
    g_genChain[0] = nullptr;
    g_genChain[1] = g_genClass + 0x30;
    const int32_t depth = 1;
    uint8_t** chain = g_genChain;
    memcpy(g_genClass + 0x38, &depth, 4);
    memcpy(g_genClass + 0x30, &chain, 8);
    static FakeMeta metaGen, metaOther;
    memcpy(metaGen.b + 0x30, &chain, 8);
    memcpy(metaGen.b + 0x38, &depth, 4);
    static uint8_t* otherChain[2] = {nullptr, nullptr};
    uint8_t** oc = otherChain;
    memcpy(metaOther.b + 0x30, &oc, 8);
    memcpy(metaOther.b + 0x38, &depth, 4);
    static FakeClass clsA, clsB, clsC;
    uint8_t* mg = metaGen.b;
    uint8_t* mo = metaOther.b;
    memcpy(clsA.b + 0x10, &mg, 8);
    memcpy(clsB.b + 0x10, &mg, 8);
    memcpy(clsC.b + 0x10, &mo, 8);  // not a generator class: memory part 0
    const uint32_t ha = 0x1234567, hb = 0x89ABCDE, hc = 0x5555;
    memcpy(clsA.b + 0x238, &ha, 4);
    memcpy(clsB.b + 0x238, &hb, 4);
    memcpy(clsC.b + 0x238, &hc, 4);

    RigSetupTestFns fns{};
    fns.vmOrig = reinterpret_cast<void*>(&FakeVmHash);
    fns.staticClass = reinterpret_cast<void*>(&FakeStaticClass);
    fns.memoryHash = reinterpret_cast<void*>(&FakeMemoryHash);
    fns.fnameCtor = reinterpret_cast<void*>(&FakeFNameCtor);
    fns.nameHash = reinterpret_cast<void*>(&FakeNameHash);
    fns.resizeTo = reinterpret_cast<void*>(&FakeResizeTo);
    fns.grow32 = reinterpret_cast<void*>(&FakeGrow32);
    fns.grow16 = reinterpret_cast<void*>(&FakeGrow16);
    fns.empty16 = reinterpret_cast<void*>(&FakeEmpty16);
    fns.free = reinterpret_cast<void*>(&FakeFree);
    fns.mapOrig[0] = reinterpret_cast<void*>(&RefJoints);
    fns.mapOrig[1] = reinterpret_cast<void*>(&RefInput);
    fns.mapOrig[2] = reinterpret_cast<void*>(&RefMorph);
    fns.mapOrig[3] = reinterpret_cast<void*>(&RefMask);
    RigSetupTestSetup(fns);
    g_nameHook = reinterpret_cast<NameHookFn>(RigSetupTestNameHook());

    std::mt19937 rng(23);
    auto makeVm = [&](FakeVm& v) {
        v.names.clear();
        for (int i = 0; i < 40; ++i) v.names.push_back(rng() % 500);
        v.codeNum = 3000 + static_cast<int32_t>(rng() % 2000);
        v.code.assign(static_cast<size_t>(v.codeNum) + 32, 0);
        v.codeOff = 16;
        for (int32_t i = 0; i < v.codeNum; ++i) v.code[16 + i] = static_cast<uint8_t>(rng());
        v.entries.assign(0xC * 3, 0);
        for (auto& x : v.entries) x = static_cast<uint8_t>(rng());
        v.branches.assign(0x18 * 2, 0);
        for (auto& x : v.branches) x = static_cast<uint8_t>(rng());
        v.ext.assign(0x28 * 2, 0);
        for (auto& x : v.ext) x = static_cast<uint8_t>(rng());
        uint8_t* ca = clsA.b;
        uint8_t* cb = clsB.b;
        memcpy(v.lit + 0x10, &ca, 8);
        memcpy(v.work + 0x10, &cb, 8);
        v.bc[0x98] = 1;
        v.Build();
    };
    bool vmOk = true;
    uint64_t c[23];
    for (int mode : {1, 2}) {
        SetRigSetup(mode, 0);
        RigSetupTestSetup(fns);
        static FakeVm a, b;
        makeVm(a);
        g_vmOrigCalls = 0;
        const uint32_t h0 = RigSetupTestVm(a.vm);  // miss
        vmOk &= h0 == FakeVmHash(a.vm) && g_vmOrigCalls == 2;
        g_vmOrigCalls = 0;
        vmOk &= RigSetupTestVm(a.vm) == h0 && g_vmOrigCalls == (mode == 1 ? 0 : 1);  // hit (verify: also the game's)
        // a copy elsewhere (another instance's VM), same alignment modulo 16: a hit
        b = a;
        b.code.assign(a.code.size() + 64, 0);
        b.codeOff = 48;
        memcpy(b.code.data() + 48, a.code.data() + 16, static_cast<size_t>(a.codeNum));
        b.Build();
        g_vmOrigCalls = 0;
        vmOk &= RigSetupTestVm(b.vm) == h0 && g_vmOrigCalls == (mode == 1 ? 0 : 1);
        // every change: a miss with the game's value
        auto expectMiss = [&](FakeVm& v, const char*) {
            v.Build();
            g_vmOrigCalls = 0;
            const uint32_t want = FakeVmHash(v.vm);
            const uint32_t got = RigSetupTestVm(v.vm);
            vmOk &= got == want && g_vmOrigCalls == 2;
            g_vmOrigCalls = 0;
            vmOk &= RigSetupTestVm(v.vm) == want && g_vmOrigCalls == (mode == 1 ? 0 : 1);
        };
        b.code[48 + 1234] ^= 1;
        expectMiss(b, "code byte");
        b.codeOff = 52;  // misaligned (aligned code)
        memmove(b.code.data() + 52, b.code.data() + 48, static_cast<size_t>(b.codeNum));
        expectMiss(b, "alignment");
        b.bc[0x98] = 0;
        expectMiss(b, "aligned flag");
        b.names[7] ^= 0x100000000ull;
        expectMiss(b, "name");
        b.entries[5] ^= 2;
        expectMiss(b, "entry");
        b.branches[0x18 + 3] ^= 4;
        expectMiss(b, "branch");
        b.ext[0x28 + 0x10] ^= 8;
        expectMiss(b, "external type");
        uint8_t* cc = clsC.b;
        memcpy(b.lit + 0x10, &cc, 8);
        expectMiss(b, "literal memory class");
        b.hasWork = false;
        expectMiss(b, "no work memory");
        b.ext[0x28 + 0x08] ^= 8;  // a field ComputeVMHash does not read: still a hit
        b.Build();
        g_vmOrigCalls = 0;
        RigSetupTestVm(b.vm);
        vmOk &= g_vmOrigCalls == (mode == 1 ? 0 : 1);
        RigSetupTestCounts(c);
        vmOk &= c[2] == 0;
    }
    Check(vmOk, "rigsetup: RigVmHashMemo returns the game's hash for equal inputs (a copy at another address too), recomputes on any "
                "input change (code, alignment, flag, name, entry, branch, external type, memory class), verify mode 0 mismatches");

    // ---- RigLogicMapCache with fakes (content keys)
    SetupVtables();
    using RefFn = void (*)(uint8_t*, uint8_t*);
    const RefFn refs[4] = {&RefJoints, &RefInput, &RefMorph, &RefMask};
    bool mapOk = true, hitsOk = true, missOk = true, classOk = true;
    int compared = 0;
    uint64_t c0[23], c1[23];
    int curMode = 1;
    auto hits = [&](const uint64_t* x, int k) { return curMode == 1 ? x[3 + k * 4] : x[19 + k]; };
    auto caps = [&](const uint64_t* x, int k) { return x[4 + k * 4]; };
    // the four maps of `rig` on h (twice: again on filled arrays), the reference functions on `ref` likewise; same arrays?
    auto runRig = [&](FakeRig& rig, FakeRig& ref, FakeHier& h, bool twice) {
        for (int k = 0; k < 4; ++k) {
            RigSetupTestMap(k, rig.data, h.b);
            if (twice) RigSetupTestMap(k, rig.data, h.b);
            refs[k](ref.data, h.b);
            if (twice) refs[k](ref.data, h.b);
        }
        ++compared;
        return SameRig(rig, ref);
    };
    for (int mode : {1, 2}) {
        curMode = mode;
        SetRigSetup(0, mode);
        RigSetupTestSetup(fns);
        for (int round = 0; round < 12; ++round) {
            const bool earlyInput = round % 3 == 1;
            FakeDna dna = MakeDna(rng, earlyInput, round % 4 == 2);
            FakeHier h1, h2;
            h1.Build(MakeKeys(rng));
            h2.Build(MakeKeys(rng));
            {  // the first head with this content: the game's functions, captured
                FakeRig first, ref;
                first.Init(&dna);
                ref.Init(&dna);
                RigSetupTestCounts(c0);
                mapOk &= runRig(first, ref, h1, false);
                RigSetupTestCounts(c1);
                for (int k = 0; k < 4; ++k) missOk &= caps(c1, k) - caps(c0, k) == 1 && hits(c1, k) == hits(c0, k);
                first.Free();
                ref.Free();
            }
            {  // the same content in another reader (a new DNA asset load = a new context): every map hits
                FakeDna copy = dna;
                copy.Finalize();
                FakeRig again, ref;
                again.Init(&copy);
                ref.Init(&copy);
                RigSetupTestCounts(c0);
                mapOk &= runRig(again, ref, h2, true);
                RigSetupTestCounts(c1);
                for (int k = 0; k < 4; ++k) hitsOk &= hits(c1, k) - hits(c0, k) == 2 && caps(c1, k) == caps(c0, k);
                again.Free();
                ref.Free();
            }
            // one change in what a map reads: that map misses (the game's function, a new capture), the others hit;
            // a change in what no map reads (a control after the early return, an unused animated map): all hit
            std::vector<std::pair<FakeDna, int>> muts;
            {
                FakeDna m = dna;
                m.joints.back() += L"x";
                muts.emplace_back(m, 0);
            }
            {
                FakeDna m = dna;
                m.controls[0] = L"CTRLX_0.ty0";
                muts.emplace_back(m, 1);
            }
            if (earlyInput) {
                FakeDna m = dna;
                m.controls.back() = L"CTRL_after.stop";
                muts.emplace_back(m, -1);
            }
            {
                FakeDna m = dna;
                m.meshes[m.morphPerLod[0][0].first] += L"m";
                muts.emplace_back(m, 2);
            }
            {
                FakeDna m = dna;
                auto& e = m.morphPerLod.back().back();
                e.second = static_cast<uint16_t>((e.second + 1) % 25);
                muts.emplace_back(m, 2);
            }
            {
                FakeDna m = dna;
                m.animPerLod[0].push_back(2);
                muts.emplace_back(m, 3);
            }
            for (uint16_t idx = 0; idx < 15; ++idx) {
                bool used = false;
                for (auto& v : dna.animPerLod)
                    for (uint16_t x : v) used |= x == idx;
                if (used) continue;
                FakeDna m = dna;
                m.animMaps[idx] = L"FRM_x.unused";
                muts.emplace_back(m, -1);
                break;
            }
            for (auto& [m, kind] : muts) {
                m.Finalize();
                FakeRig rig, ref;
                rig.Init(&m);
                ref.Init(&m);
                RigSetupTestCounts(c0);
                mapOk &= runRig(rig, ref, h2, false);
                RigSetupTestCounts(c1);
                for (int k = 0; k < 4; ++k) {
                    if (k == kind) missOk &= caps(c1, k) - caps(c0, k) == 1 && hits(c1, k) == hits(c0, k);
                    else hitsOk &= hits(c1, k) - hits(c0, k) == 1 && caps(c1, k) == caps(c0, k);
                }
                rig.Free();
                ref.Free();
            }
        }
        RigSetupTestCounts(c1);
        for (int k = 0; k < 4; ++k) mapOk &= c1[5 + k * 4] == 0 && c1[6 + k * 4] == 0;  // no mismatch, nothing rejected
    }
    Check(mapOk, "rigsetup: RigLogicMapCache's results on other readers and hierarchies equal the reference map functions' arrays "
                 "(values, counts, capacities; early returns in the control and mask maps), verify mode 0 mismatches");
    Check(hitsOk, "rigsetup: RigLogicMapCache keys on content: the same DNA content in another reader / context hits, as do changes "
                  "the stock code does not read (a control after the early return, an unused animated map)");
    Check(missOk, "rigsetup: RigLogicMapCache: any change the stock code reads (a joint name, a control, a mesh name, a blend shape "
                  "mapping, an LOD view) misses that map only");
    {  // reader classes: dna strings that disagree with the FString getters -> refused; an Unwrap of unknown shape -> unused
        curMode = 1;
        SetRigSetup(0, 1);
        RigSetupTestSetup(fns);
        FakeDna dna = MakeDna(rng, false, false);
        FakeHier h;
        h.Build(MakeKeys(rng));
        FakeRig liar, cpp, good, good2, ref;
        liar.Init(&dna, g_vtbl, g_defVtblLiar);
        cpp.Init(&dna, g_vtblCpp, g_defVtbl);
        good.Init(&dna);
        good2.Init(&dna);
        RigSetupTestCounts(c0);
        ref.Init(&dna);
        classOk &= runRig(liar, ref, h, true);
        ref.Free();
        ref.Init(&dna);
        classOk &= runRig(cpp, ref, h, true);
        ref.Free();
        RigSetupTestCounts(c1);
        for (int k = 0; k < 4; ++k) classOk &= caps(c1, k) == caps(c0, k) && hits(c1, k) == hits(c0, k);
        classOk &= RigSetupTestReaderClasses(-1) == 1 && g_cppUnwraps == 0;
        ref.Init(&dna);
        classOk &= runRig(good, ref, h, false);
        ref.Free();
        ref.Init(&dna);
        classOk &= runRig(good2, ref, h, false);
        ref.Free();
        RigSetupTestCounts(c0);
        for (int k = 0; k < 4; ++k) classOk &= caps(c0, k) - caps(c1, k) == 1 && hits(c0, k) - hits(c1, k) == 1;
        classOk &= RigSetupTestReaderClasses(1) == 1;
        liar.Free();
        cpp.Free();
        good.Free();
        good2.Free();
    }
    Check(classOk, "rigsetup: RigLogicMapCache refuses a reader class whose dna strings differ from the FString getters and one whose "
                   "Unwrap has an unknown code shape (results stock), and still caches the good class");
    SetRigSetup(0, 0);
    Log("[rigsetup] self-test: %d rigs compared", compared);
}

}  // namespace rsu
