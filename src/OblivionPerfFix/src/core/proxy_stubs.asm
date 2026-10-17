; version.dll proxy: each export jumps to the real System32\version.dll function.
; The g_real_* pointers are resolved in DllMain (see proxy.cpp) before any import is called.

STUB MACRO name
EXTERN g_real_&name&:QWORD
PUBLIC Proxy_&name&
Proxy_&name& PROC
    jmp QWORD PTR [g_real_&name&]
Proxy_&name& ENDP
ENDM

.code
STUB GetFileVersionInfoA
STUB GetFileVersionInfoByHandle
STUB GetFileVersionInfoExA
STUB GetFileVersionInfoExW
STUB GetFileVersionInfoSizeA
STUB GetFileVersionInfoSizeExA
STUB GetFileVersionInfoSizeExW
STUB GetFileVersionInfoSizeW
STUB GetFileVersionInfoW
STUB VerFindFileA
STUB VerFindFileW
STUB VerInstallFileA
STUB VerInstallFileW
STUB VerLanguageNameA
STUB VerLanguageNameW
STUB VerQueryValueA
STUB VerQueryValueW
END
