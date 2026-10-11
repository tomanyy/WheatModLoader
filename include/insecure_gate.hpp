#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace modloader {

enum class GateResult { Allowed, MissingInsecure, ConflictingSecure };

struct CmdToken {
    std::wstring text;
    bool quoted = false;
};

inline std::vector<CmdToken> split_command_line(std::wstring_view s) {
    std::vector<CmdToken> out;
    CmdToken cur;
    bool in_quotes = false, have = false;
    for (wchar_t c : s) {
        if (c == L'"') { in_quotes = !in_quotes; cur.quoted = true; have = true; continue; }
        if (!in_quotes && (c == L' ' || c == L'\t')) {
            if (have) { out.push_back(std::move(cur)); cur = {}; have = false; }
            continue;
        }
        cur.text.push_back(c);
        have = true;
    }
    if (have) out.push_back(std::move(cur));
    return out;
}

inline bool equals_ascii_nocase(std::wstring_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        wchar_t x = a[i];
        if (x >= L'A' && x <= L'Z') x = static_cast<wchar_t>(x - L'A' + L'a');
        if (x != static_cast<wchar_t>(b[i])) return false;
    }
    return true;
}

inline GateResult check_launch_gate(std::wstring_view command_line) {
    bool insecure = false;
    for (const auto& t : split_command_line(command_line)) {
        if (equals_ascii_nocase(t.text, "-secure")) return GateResult::ConflictingSecure;
        if (!t.quoted && equals_ascii_nocase(t.text, "-insecure")) insecure = true;
    }
    return insecure ? GateResult::Allowed : GateResult::MissingInsecure;
}

}  // namespace modloader
