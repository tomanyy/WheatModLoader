#pragma once
#include <string>
#include <string_view>
#include <set>

struct lua_State;

namespace modloader {

struct ConsoleValue {
    enum class Kind { boolean, integer, number, text };
    Kind kind = Kind::integer;
    bool boolean = false;
    long long integer = 0;
    double number = 0.0;
    std::string text;
};

enum class ConsoleStatus { ok, unavailable, notFound, unsupportedType };

class ConsoleBackend {
public:
    virtual ~ConsoleBackend() = default;
    virtual bool available() const = 0;
    virtual ConsoleStatus read(const std::string& name, bool wantDefault, ConsoleValue& out) = 0;
};

bool isReadableConVar(std::string_view name);
void pushConsoleApi(lua_State* L, ConsoleBackend* backend, const std::set<std::string>& allowList = {});

}  // namespace modloader
