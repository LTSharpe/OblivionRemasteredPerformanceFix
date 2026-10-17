// version.dll proxy: resolves the real System32 functions that the asm stubs jump to.
#include "common.h"

#define PROXY_EXPORTS(X)                                                                    \
    X(GetFileVersionInfoA) X(GetFileVersionInfoByHandle) X(GetFileVersionInfoExA)            \
    X(GetFileVersionInfoExW) X(GetFileVersionInfoSizeA) X(GetFileVersionInfoSizeExA)         \
    X(GetFileVersionInfoSizeExW) X(GetFileVersionInfoSizeW) X(GetFileVersionInfoW)           \
    X(VerFindFileA) X(VerFindFileW) X(VerInstallFileA) X(VerInstallFileW)                    \
    X(VerLanguageNameA) X(VerLanguageNameW) X(VerQueryValueA) X(VerQueryValueW)

#define DECLARE_REAL(name) extern "C" FARPROC g_real_##name = nullptr;
PROXY_EXPORTS(DECLARE_REAL)

namespace opf {

// Stand-in for an export that could not be resolved: every version.dll function
// reports failure as 0 (FALSE or a zero size), so callers see an ordinary error.
static uintptr_t ProxyUnavailable()
{
    SetLastError(ERROR_PROC_NOT_FOUND);
    return 0;
}

bool ResolveVersionProxy()
{
#define FALLBACK(name) g_real_##name = reinterpret_cast<FARPROC>(&ProxyUnavailable);
    PROXY_EXPORTS(FALLBACK)

    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n > MAX_PATH - 16) return false;
    wcscat_s(path, L"\\version.dll");
    // version.dll only depends on kernel32/kernelbase/ntdll, so loading it from DllMain is safe.
    HMODULE real = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!real) return false;
    bool ok = true;
#define RESOLVE(name)                                                   \
    if (FARPROC p = GetProcAddress(real, #name)) g_real_##name = p;     \
    else ok = false;
    PROXY_EXPORTS(RESOLVE)
    return ok;
}

}  // namespace opf
