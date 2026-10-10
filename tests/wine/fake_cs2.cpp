// Stand-in for cs2.exe: imports version.dll (so Wine's override loads our proxy) and calls one
// forwarded export, then lingers long enough for the loader thread to do its work.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winver.h>
#include <cstdio>

int main() {
    wchar_t name[128] = {};
    const UINT n = VerLanguageNameW(0x0409, name, 128);  // pure function: easy to check the forwarding
    std::printf("VerLanguageNameW -> %u '%ls'\n", n, name);
    std::fflush(stdout);
    Sleep(1500);
    return n ? 0 : 3;
}
