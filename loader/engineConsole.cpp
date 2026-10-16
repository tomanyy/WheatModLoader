#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "engineConsole.hpp"

namespace fs = std::filesystem;

namespace modloader {
namespace {

constexpr int slotFindConVar = 11;
constexpr int slotFindFirstConVar = 12;
constexpr int slotFindNextConVar = 13;
constexpr int slotGetConVarData = 41;
constexpr int vtableSlotsToCheck = 48;

constexpr std::size_t offDefault = 8;
constexpr std::size_t offType = 40;
constexpr std::size_t offFlags = 48;
// The live-game dump showed offset 88 holding current values, while offset 96
// often held the default/cached value (for example fps_max=0 vs default 400).
constexpr std::size_t currentValueOffset = 88;
constexpr std::size_t diagnosticValueOffsets[] = {88, 96};
constexpr std::size_t dataReadBytes = 112;
constexpr std::size_t callbackIdOffset = 56;
constexpr std::size_t conVarValuesOffset = 64;
constexpr std::size_t cvarCallbackTableOffset = 0x80;
constexpr std::uint16_t invalidAccessIndex = 0xFFFF;
constexpr int typeCount = 16;
constexpr int minCalibrationMatches = 3;

enum ConVarType : std::int16_t { tBool, tInt16, tUInt16, tInt32, tUInt32, tInt64, tUInt64, tFloat32, tFloat64, tString };

enum class Abi { unknown, hiddenPointer, registerReturn };

using FindHiddenFn = void* (*)(void*, std::uint64_t*, const char*, bool);
using FindRegisterFn = std::uint64_t (*)(void*, const char*, bool);
using FirstHiddenFn = void* (*)(void*, std::uint64_t*);
using FirstRegisterFn = std::uint64_t (*)(void*);
using NextHiddenFn = void* (*)(void*, std::uint64_t*, std::uint64_t);
using NextRegisterFn = std::uint64_t (*)(void*, std::uint64_t);
using GetDataFn = void* (*)(void*, std::uint64_t);
using CreateInterfaceFn = void* (*)(const char*, int*);

const char* const calibrationNames[] = {"fps_max", "cl_showfps", "sensitivity", "volume", "cl_crosshairsize", "cl_crosshair_gap",
                                        "cl_crosshairthickness", "viewmodel_fov", "cl_hud_color", "cl_radar_scale", "cl_language"};
const char* const unknownName = "wheat_no_such_convar_9f3";

bool isReadableProtection(DWORD protect) {
    if (protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY);
}

bool readable(const void* pointer, std::size_t size) {
    std::uintptr_t cursor = reinterpret_cast<std::uintptr_t>(pointer);
    if (cursor < 0x10000) return false;
    const std::uintptr_t end = cursor + size;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof info)) return false;
        if (info.State != MEM_COMMIT || !isReadableProtection(info.Protect)) return false;
        cursor = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    }
    return true;
}

bool executable(const void* pointer) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(pointer, &info, sizeof info)) return false;
    return info.State == MEM_COMMIT && (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

HMODULE moduleOf(const void* address) {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(address), &module);
    return module;
}

std::string moduleName(const void* address) {
    HMODULE module = moduleOf(address);
    if (!module) return "?";
    char path[MAX_PATH] = {};
    GetModuleFileNameA(module, path, MAX_PATH);
    const char* slash = std::strrchr(path, '\\');
    char buffer[MAX_PATH + 32];
    std::snprintf(buffer, sizeof buffer, "%s+0x%llx", slash ? slash + 1 : path,
                  static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(module)));
    return buffer;
}

bool readCString(const char* pointer, std::size_t capacity, std::string& out) {
    out.clear();
    for (std::size_t i = 0; i < capacity; ++i) {
        const char* at = pointer + i;
        if ((i == 0 || (reinterpret_cast<std::uintptr_t>(at) & 0xFFF) == 0) && !readable(at, 1)) return false;
        if (*at == 0) return true;
        out.push_back(*at);
    }
    return false;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::size_t typeSize(std::int16_t type) {
    switch (type) {
        case tBool: return 1;
        case tInt16: case tUInt16: return 2;
        case tInt32: case tUInt32: case tFloat32: return 4;
        case tInt64: case tUInt64: case tFloat64: return 8;
        default: return 0;
    }
}

std::string bytesAt(const unsigned char* data, std::size_t size) {
    char byte[4];
    std::string result;
    for (std::size_t i = 0; i < size; ++i) {
        std::snprintf(byte, sizeof byte, "%02X", data[i]);
        if (!result.empty()) result += ' ';
        result += byte;
    }
    return result;
}

ConsoleStatus convert(std::int16_t type, const unsigned char* source, ConsoleValue& out) {
    if (type == tString) {
        if (!readable(source, sizeof(char*))) return ConsoleStatus::unavailable;
        const char* text = nullptr;
        std::memcpy(&text, source, sizeof text);
        std::string value;
        if (!text || !readCString(text, 512, value)) return ConsoleStatus::unsupportedType;
        out.kind = ConsoleValue::Kind::text;
        out.text = std::move(value);
        return ConsoleStatus::ok;
    }
    const std::size_t size = typeSize(type);
    if (size == 0) return ConsoleStatus::unsupportedType;
    if (!readable(source, size)) return ConsoleStatus::unavailable;
    switch (type) {
        case tBool: out.kind = ConsoleValue::Kind::boolean; out.boolean = *source != 0; break;
        case tInt16: { std::int16_t v; std::memcpy(&v, source, 2); out.kind = ConsoleValue::Kind::integer; out.integer = v; break; }
        case tUInt16: { std::uint16_t v; std::memcpy(&v, source, 2); out.kind = ConsoleValue::Kind::integer; out.integer = v; break; }
        case tInt32: { std::int32_t v; std::memcpy(&v, source, 4); out.kind = ConsoleValue::Kind::integer; out.integer = v; break; }
        case tUInt32: { std::uint32_t v; std::memcpy(&v, source, 4); out.kind = ConsoleValue::Kind::integer; out.integer = v; break; }
        case tInt64: { std::int64_t v; std::memcpy(&v, source, 8); out.kind = ConsoleValue::Kind::integer; out.integer = v; break; }
        case tUInt64: { std::uint64_t v; std::memcpy(&v, source, 8); out.kind = ConsoleValue::Kind::number; out.number = static_cast<double>(v); break; }
        case tFloat32: { float v; std::memcpy(&v, source, 4); out.kind = ConsoleValue::Kind::number; out.number = v; break; }
        case tFloat64: { double v; std::memcpy(&v, source, 8); out.kind = ConsoleValue::Kind::number; out.number = v; break; }
    }
    return ConsoleStatus::ok;
}

}  // namespace

struct EngineConsole::Impl {
    std::atomic<bool> ready{false};
    std::mutex mutex;
    void* cvar = nullptr;
    void** vtable = nullptr;
    Abi abi = Abi::unknown;
    std::size_t valueOffset = 0;
    std::ofstream reportFile;

    void say(const std::string& line) {
        if (!reportFile) return;
        reportFile << line << "\n";
        reportFile.flush();
    }

    bool findRef(const char* name, std::uint64_t& ref) {
        if (abi == Abi::hiddenPointer) {
            std::uint64_t out = invalidAccessIndex;
            reinterpret_cast<FindHiddenFn>(vtable[slotFindConVar])(cvar, &out, name, false);
            ref = out;
        } else {
            ref = reinterpret_cast<FindRegisterFn>(vtable[slotFindConVar])(cvar, name, false);
        }
        return (ref & 0xFFFF) != invalidAccessIndex;
    }

    const unsigned char* getData(std::uint64_t ref) {
        return static_cast<const unsigned char*>(reinterpret_cast<GetDataFn>(vtable[slotGetConVarData])(cvar, ref));
    }

    bool validData(const unsigned char* data, const std::string& expectedName, std::string& why) {
        if (!data || !readable(data, dataReadBytes)) {
            why = "convar data pointer is not readable";
            return false;
        }
        const char* namePointer = nullptr;
        std::memcpy(&namePointer, data, sizeof namePointer);
        std::string actual;
        if (!namePointer || !readCString(namePointer, 128, actual)) {
            why = "name pointer at offset 0 is not a readable string";
            return false;
        }
        if (lower(actual) != lower(expectedName)) {
            why = "name at offset 0 is '" + actual + "', expected '" + expectedName + "'";
            return false;
        }
        std::int16_t type;
        std::memcpy(&type, data + offType, 2);
        if (type < 0 || type >= typeCount) {
            why = "type at offset 40 is out of range (" + std::to_string(type) + ")";
            return false;
        }
        const unsigned char* defaultValue = nullptr;
        std::memcpy(&defaultValue, data + offDefault, sizeof defaultValue);
        if (defaultValue && !readable(defaultValue, 16)) {
            why = "default value pointer at offset 8 is not readable";
            return false;
        }
        return true;
    }

    bool detectAbi(const char* probeName) {
        std::uint64_t out = invalidAccessIndex;
        void* returned = reinterpret_cast<FindHiddenFn>(vtable[slotFindConVar])(cvar, &out, probeName, false);
        abi = returned == &out ? Abi::hiddenPointer : Abi::registerReturn;
        say(std::string("FindConVar calling convention: ") + (abi == Abi::hiddenPointer ? "result returned through a hidden pointer" : "result returned in a register"));
        return true;
    }

    void dumpConVars(const fs::path& file) {
        say("about to enumerate convars (vtable slots 12 and 13)");
        std::ofstream out(file, std::ios::trunc);
        std::uint64_t ref = invalidAccessIndex;
        if (abi == Abi::hiddenPointer) {
            std::uint64_t first = invalidAccessIndex;
            reinterpret_cast<FirstHiddenFn>(vtable[slotFindFirstConVar])(cvar, &first);
            ref = first;
        } else {
            ref = reinterpret_cast<FirstRegisterFn>(vtable[slotFindFirstConVar])(cvar);
        }
        int written = 0;
        for (; (ref & 0xFFFF) != invalidAccessIndex && written < 20000; ++written) {
            const unsigned char* data = getData(ref);
            if (data && readable(data, dataReadBytes)) {
                const char* namePointer = nullptr;
                std::memcpy(&namePointer, data, sizeof namePointer);
                std::string name;
                std::int16_t type;
                std::uint64_t flags;
                std::memcpy(&type, data + offType, 2);
                std::memcpy(&flags, data + offFlags, 8);
                if (namePointer && readCString(namePointer, 128, name)) {
                    char line[256];
                    std::snprintf(line, sizeof line, "%s\t%d\t0x%llx\n", name.c_str(), type, static_cast<unsigned long long>(flags));
                    out << line;
                }
            }
            std::uint64_t next = invalidAccessIndex;
            if (abi == Abi::hiddenPointer) {
                reinterpret_cast<NextHiddenFn>(vtable[slotFindNextConVar])(cvar, &next, ref);
                ref = next;
            } else {
                ref = reinterpret_cast<NextRegisterFn>(vtable[slotFindNextConVar])(cvar, ref);
            }
        }
        say("convar dump: " + std::to_string(written) + " entries written to " + file.filename().string());
    }

    bool calibrate(const fs::path& wheatDir);
    bool discover();
};

bool EngineConsole::Impl::discover() {
    HMODULE tier0 = GetModuleHandleW(L"tier0.dll");
    if (!tier0) {
        say("FAILED: tier0.dll is not loaded");
        return false;
    }
    char path[MAX_PATH] = {};
    GetModuleFileNameA(tier0, path, MAX_PATH);
    say(std::string("tier0.dll: ") + path);

    auto createInterface = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(tier0, "CreateInterface"));
    if (!createInterface) {
        say("FAILED: tier0.dll does not export CreateInterface");
        return false;
    }
    cvar = createInterface("VEngineCvar007", nullptr);
    if (!cvar || !readable(cvar, sizeof(void*))) {
        say("FAILED: CreateInterface(\"VEngineCvar007\") returned no usable object");
        return false;
    }
    vtable = *reinterpret_cast<void***>(cvar);
    if (!readable(vtable, sizeof(void*) * vtableSlotsToCheck)) {
        say("FAILED: the interface's vtable is not readable");
        return false;
    }
    if (moduleOf(vtable) != tier0) {
        say("FAILED: the vtable is not inside tier0.dll (" + moduleName(vtable) + ")");
        return false;
    }
    char line[160];
    std::snprintf(line, sizeof line, "ICvar object %p, vtable %p (%s)", cvar, static_cast<void*>(vtable), moduleName(vtable).c_str());
    say(line);
    for (int slot = 0; slot < vtableSlotsToCheck; ++slot) {
        if (vtable[slot]) {
            say("vtable[" + std::to_string(slot) + "] -> " + moduleName(vtable[slot]) +
                (executable(vtable[slot]) ? " (code)" : " (data)"));
        } else {
            say("vtable[" + std::to_string(slot) + "] -> null");
        }
    }
    for (int slot : {slotFindConVar, slotFindFirstConVar, slotFindNextConVar, slotGetConVarData}) {
        if (!vtable[slot] || !executable(vtable[slot]) || moduleOf(vtable[slot]) != tier0) {
            say("FAILED: vtable slot " + std::to_string(slot) + " is not code inside tier0.dll");
            return false;
        }
        say("vtable[" + std::to_string(slot) + "] -> " + moduleName(vtable[slot]));
    }
    return true;
}

bool EngineConsole::Impl::calibrate(const fs::path& wheatDir) {
    say("about to call FindConVar (vtable slot 11); if the game dies here, the last line above is the cause");
    detectAbi(calibrationNames[0]);
    say("FindConVar returned; about to call GetConVarData (vtable slot 41)");

    std::uint64_t ref = 0;
    if (findRef(unknownName, ref)) {
        say("FAILED: FindConVar returned a valid reference for a name that cannot exist");
        return false;
    }

    struct Found {
        std::string name;
        const unsigned char* data;
        std::int16_t type;
    };
    std::vector<Found> found;
    int layoutFailures = 0;
    for (const char* name : calibrationNames) {
        if (!findRef(name, ref)) {
            say(std::string("  ") + name + ": not found");
            continue;
        }
        const unsigned char* data = getData(ref);
        std::string why;
        if (!validData(data, name, why)) {
            say(std::string("  ") + name + ": LAYOUT MISMATCH - " + why);
            ++layoutFailures;
            continue;
        }
        if (std::strcmp(name, "fps_max") == 0) {
            say("fps_max data record bytes (read-only inspection): " + bytesAt(data, dataReadBytes));
            std::uint32_t callbackId = 0;
            std::memcpy(&callbackId, data + callbackIdOffset, sizeof callbackId);
            say("fps_max callback id at offset 56: " + std::to_string(callbackId));
            for (std::size_t offset = conVarValuesOffset; offset <= 96; offset += 8) {
                say("fps_max value slot at offset " + std::to_string(offset) + ": " +
                    bytesAt(data + offset, sizeof(std::uint64_t)));
            }

        }
        std::int16_t type;
        std::memcpy(&type, data + offType, 2);
        found.push_back({name, data, type});
        say(std::string("  ") + name + ": found, type " + std::to_string(type));
    }
    // Inspect the callback metadata for the calibration variables. The table
    // entry is data only; discovery never calls an engine callback.
    void** callbackTable = nullptr;
    const auto* cvarBytes = static_cast<const unsigned char*>(cvar);
    if (readable(cvarBytes + cvarCallbackTableOffset, sizeof callbackTable)) {
        std::memcpy(&callbackTable, cvarBytes + cvarCallbackTableOffset, sizeof callbackTable);
    }
    say(std::string("CCvar callback table candidate: ") +
        (callbackTable ? moduleName(callbackTable) : "null"));
    for (const Found& f : found) {
        std::uint32_t callbackId = 0;
        std::memcpy(&callbackId, f.data + callbackIdOffset, sizeof callbackId);
        say(f.name + " callback id: " + std::to_string(callbackId));
        if (!callbackTable || callbackId == 0 || callbackId >= 4096 ||
            !readable(callbackTable + static_cast<std::size_t>(callbackId) * 3, sizeof(void*) * 3)) continue;
        const void* const* entry = callbackTable + static_cast<std::size_t>(callbackId) * 3;
        for (int i = 0; i < 3; ++i) {
            say(f.name + " callback entry[" + std::to_string(i) + "] -> " +
                (entry[i] ? moduleName(entry[i]) + (executable(entry[i]) ? " (code)" : " (data)") : "null"));
        }
    }
    if (layoutFailures > 0) {
        say("FAILED: " + std::to_string(layoutFailures) + " convar(s) did not match the expected ConVarData layout");
        return false;
    }
    if (static_cast<int>(found.size()) < minCalibrationMatches) {
        say("FAILED: only " + std::to_string(found.size()) + " calibration convars found (need " + std::to_string(minCalibrationMatches) + ")");
        return false;
    }

    int compared = 0;
    for (const Found& f : found) {
        const std::size_t size = typeSize(f.type);
        const unsigned char* defaultValue = nullptr;
        std::memcpy(&defaultValue, f.data + offDefault, sizeof defaultValue);
        if (size == 0 || !defaultValue) continue;
        ++compared;
        say("  " + f.name + " candidate bytes (" + std::to_string(size) + " bytes): offset 88 [" +
            bytesAt(f.data + diagnosticValueOffsets[0], size) + "], offset 96 [" +
            bytesAt(f.data + diagnosticValueOffsets[1], size) + "], default [" + bytesAt(defaultValue, size) + "]");
    }
    say("compared " + std::to_string(compared) + " numeric convars; byte comparison with defaults is diagnostic only");
    valueOffset = currentValueOffset;
    say("value offset: " + std::to_string(valueOffset));

    if (fs::exists(wheatDir / "dumpConvars")) dumpConVars(wheatDir / "convars.txt");
    return true;
}

EngineConsole::EngineConsole() : impl(std::make_unique<Impl>()) {}
EngineConsole::~EngineConsole() = default;

bool EngineConsole::available() const { return impl->ready.load(); }

bool EngineConsole::initialize(const fs::path& wheatDir) {
    Impl& im = *impl;
    std::lock_guard<std::mutex> lock(im.mutex);
    im.reportFile.open(wheatDir / "consoleReport.txt", std::ios::trunc);
    bool ok = false;
    try {
        if (im.discover() && im.calibrate(wheatDir)) {
            ok = true;
            im.say("RESULT: console API enabled (read-only)");
        } else {
            im.say("RESULT: console API stays off");
        }
    } catch (const std::exception& e) {
        im.say(std::string("FAILED: exception ") + e.what());
    }
    im.reportFile.close();
    im.ready.store(ok);
    return ok;
}

ConsoleStatus EngineConsole::read(const std::string& name, bool wantDefault, ConsoleValue& out) {
    Impl& im = *impl;
    if (!im.ready.load()) return ConsoleStatus::unavailable;
    std::lock_guard<std::mutex> lock(im.mutex);
    std::uint64_t ref = 0;
    if (!im.findRef(name.c_str(), ref)) return ConsoleStatus::notFound;
    const unsigned char* data = im.getData(ref);
    std::string why;
    if (!im.validData(data, name, why)) return ConsoleStatus::notFound;

    std::int16_t type;
    std::memcpy(&type, data + offType, 2);
    const unsigned char* source = data + im.valueOffset;
    if (wantDefault) {
        std::memcpy(&source, data + offDefault, sizeof source);
        if (!source) return ConsoleStatus::unsupportedType;
    }
    return convert(type, source, out);
}

}  // namespace modloader
