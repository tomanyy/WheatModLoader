#include "insecure_gate.hpp"
#include <iostream>
using namespace modloader;

static int failed = 0;
static void expect(const wchar_t* cmd, GateResult want, const char* label) {
    const bool ok = check_launch_gate(cmd) == want;
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << label << "\n";
    if (!ok) ++failed;
}

int main() {
    std::cout << "== launch gate ==\n";
    expect(L"\"C:\\Program Files (x86)\\Steam\\cs2.exe\" -insecure -novid", GateResult::Allowed, "plain -insecure, quoted exe path");
    expect(L"cs2.exe -INSECURE", GateResult::Allowed, "case-insensitive");
    expect(L"cs2.exe  -novid\t-insecure  +map de_dust2", GateResult::Allowed, "tabs / extra spaces");
    expect(L"cs2.exe", GateResult::MissingInsecure, "no flag");
    expect(L"", GateResult::MissingInsecure, "empty command line");
    expect(L"cs2.exe -insecure2", GateResult::MissingInsecure, "longer token is not the flag");
    expect(L"cs2.exe -insecur", GateResult::MissingInsecure, "prefix is not the flag");
    expect(L"cs2.exe +insecure", GateResult::MissingInsecure, "+insecure is a convar, not the flag");
    expect(L"cs2.exe -x \"-insecure\"", GateResult::MissingInsecure, "quoted -insecure is refused");
    expect(L"cs2.exe -novid -x=-insecure", GateResult::MissingInsecure, "flag embedded in another arg is refused");
    expect(L"cs2.exe -insecure -secure", GateResult::ConflictingSecure, "-secure vetoes -insecure");
    expect(L"cs2.exe -insecure \"-secure\"", GateResult::ConflictingSecure, "quoted -secure still vetoes");
    std::cout << "\n" << (failed ? "FAILED\n" : "all passed\n");
    return failed ? 1 : 0;
}
