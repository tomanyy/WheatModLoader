#include "engineConsole.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

namespace modloader {
namespace {

constexpr int slotFindConVar = 11;
constexpr int slotGetConVarData = 41;
constexpr std::size_t offDefault = 8;
constexpr std::size_t offType = 40;
constexpr std::size_t currentValueOffset = 88;
constexpr std::size_t dataReadBytes = 112;
constexpr std::uint16_t invalidAccessIndex = 0xFFFF;
constexpr int typeCount = 16;
constexpr std::string_view probes[] = {"fps_max", "cl_showfps", "sensitivity", "volume", "cl_crosshairsize",
                                       "viewmodel_fov", "cl_hud_color", "cl_radar_scale", "cl_language"};
constexpr char missingName[] = "wheat_no_such_convar_9f3";

using CreateInterfaceFn = void* (*)(const char*, int*);
using FindHiddenFn = void* (*)(void*, std::uint64_t*, const char*, bool);
using FindRegisterFn = std::uint64_t (*)(void*, const char*, bool);
using GetConVarDataFn = void* (*)(void*, std::uint64_t);

enum class FindAbi { unknown, hiddenPointer, registerReturn };

bool readable(const void* pointer, std::size_t size) {
    if (!pointer || size == 0) return pointer != nullptr;
    const auto begin = reinterpret_cast<std::uintptr_t>(pointer);
    if (begin < 0x10000 || begin + size < begin) return false;
    const auto end = begin + size;
    std::ifstream maps("/proc/self/maps");
    std::string line;
    auto cursor = begin;
    while (cursor < end && std::getline(maps, line)) {
        std::uintptr_t low = 0, high = 0;
        char permissions[5]{};
        if (std::sscanf(line.c_str(), "%lx-%lx %4s", &low, &high, permissions) != 3) continue;
        if (cursor < low || cursor >= high || permissions[0] != 'r') continue;
        cursor = std::min(end, high);
        if (cursor == end) return true;
    }
    return false;
}

bool executableInKnownModule(const void* pointer) {
    Dl_info info{};
    if (!pointer || !dladdr(pointer, &info) || !info.dli_fname) return false;
    std::string path(info.dli_fname);
    if (path.find("libengine2.so") == std::string::npos && path.find("libtier0.so") == std::string::npos) return false;
    std::ifstream maps("/proc/self/maps");
    std::string line;
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    while (std::getline(maps, line)) {
        std::uintptr_t low = 0, high = 0;
        char permissions[5]{};
        if (std::sscanf(line.c_str(), "%lx-%lx %4s", &low, &high, permissions) == 3 &&
            address >= low && address < high && permissions[2] == 'x') return true;
    }
    return false;
}

bool readCString(const char* pointer, std::size_t capacity, std::string& out) {
    out.clear();
    for (std::size_t i = 0; i < capacity; ++i) {
        if ((i == 0 || (reinterpret_cast<std::uintptr_t>(pointer + i) & 0xFFF) == 0) && !readable(pointer + i, 1)) return false;
        if (pointer[i] == '\0') return true;
        out.push_back(pointer[i]);
    }
    return false;
}

std::size_t typeSize(std::int16_t type) {
    switch (type) {
        case 0: return 1;
        case 1: case 2: return 2;
        case 3: case 4: case 7: return 4;
        case 5: case 6: case 8: return 8;
        default: return 0;
    }
}

ConsoleStatus convert(std::int16_t type, const unsigned char* source, ConsoleValue& out) {
    if (type == 9) {
        const char* text = nullptr;
        if (!readable(source, sizeof(text))) return ConsoleStatus::unavailable;
        std::memcpy(&text, source, sizeof(text));
        if (!text || !readCString(text, 512, out.text)) return ConsoleStatus::unsupportedType;
        out.kind = ConsoleValue::Kind::text;
        return ConsoleStatus::ok;
    }
    const auto size = typeSize(type);
    if (!size || !readable(source, size)) return size ? ConsoleStatus::unavailable : ConsoleStatus::unsupportedType;
    switch (type) {
        case 0: out.kind = ConsoleValue::Kind::boolean; out.boolean = *source != 0; break;
        case 1: { std::int16_t v; std::memcpy(&v, source, 2); out.kind = ConsoleValue::Kind::integer; out.integer = v; break; }
        case 2: { std::uint16_t v; std::memcpy(&v, source, 2); out.kind = ConsoleValue::Kind::integer; out.integer = v; break; }
        case 3: { std::int32_t v; std::memcpy(&v, source, 4); out.kind = ConsoleValue::Kind::integer; out.integer = v; break; }
        case 4: { std::uint32_t v; std::memcpy(&v, source, 4); out.kind = ConsoleValue::Kind::integer; out.integer = v; break; }
        case 5: { std::int64_t v; std::memcpy(&v, source, 8); out.kind = ConsoleValue::Kind::integer; out.integer = v; break; }
        case 6: { std::uint64_t v; std::memcpy(&v, source, 8); out.kind = ConsoleValue::Kind::number; out.number = static_cast<double>(v); break; }
        case 7: { float v; std::memcpy(&v, source, 4); out.kind = ConsoleValue::Kind::number; out.number = v; break; }
        case 8: { double v; std::memcpy(&v, source, 8); out.kind = ConsoleValue::Kind::number; out.number = v; break; }
    }
    return ConsoleStatus::ok;
}

}  // namespace

struct EngineConsole::Impl {
    std::atomic<bool> ready{false};
    std::mutex mutex;
    void* cvar = nullptr;
    void** vtable = nullptr;
    FindAbi findAbi = FindAbi::unknown;
    std::ofstream report;

    void say(const std::string& message) { if (report) { report << message << '\n'; report.flush(); } }

    bool find(const char* name, std::uint64_t& ref) {
        if (findAbi == FindAbi::hiddenPointer) {
            std::uint64_t result = invalidAccessIndex;
            reinterpret_cast<FindHiddenFn>(vtable[slotFindConVar])(cvar, &result, name, false);
            ref = result;
        } else {
            ref = reinterpret_cast<FindRegisterFn>(vtable[slotFindConVar])(cvar, name, false);
        }
        return (ref & 0xFFFF) != invalidAccessIndex;
    }

    void detectFindAbi(const char* probeName) {
        std::uint64_t result = invalidAccessIndex;
        void* returned = reinterpret_cast<FindHiddenFn>(vtable[slotFindConVar])(cvar, &result, probeName, false);
        findAbi = returned == &result ? FindAbi::hiddenPointer : FindAbi::registerReturn;
        say(std::string("FindConVar calling convention: ") +
            (findAbi == FindAbi::hiddenPointer ? "result returned through a hidden pointer" : "result returned in a register"));
    }

    const unsigned char* data(std::uint64_t ref) {
        return static_cast<const unsigned char*>(reinterpret_cast<GetConVarDataFn>(vtable[slotGetConVarData])(cvar, ref));
    }

    bool valid(const unsigned char* record, const std::string& name, std::int16_t& type) {
        if (!readable(record, dataReadBytes)) return false;
        const char* actualName = nullptr;
        std::memcpy(&actualName, record, sizeof(actualName));
        std::string actual;
        if (!actualName || !readCString(actualName, 128, actual) || actual != name) return false;
        std::memcpy(&type, record + offType, sizeof(type));
        if (type < 0 || type >= typeCount) return false;
        const unsigned char* defaultValue = nullptr;
        std::memcpy(&defaultValue, record + offDefault, sizeof(defaultValue));
        return !defaultValue || readable(defaultValue, 16);
    }
};

EngineConsole::EngineConsole() : impl(std::make_unique<Impl>()) {}
EngineConsole::~EngineConsole() = default;
bool EngineConsole::available() const { return impl->ready.load(); }

bool EngineConsole::initialize(const std::filesystem::path& wheatDir) {
    Impl& im = *impl;
    std::lock_guard<std::mutex> lock(im.mutex);
    im.report.open(wheatDir / "consoleReport.txt", std::ios::trunc);
    bool ok = false;
    try {
        {
            void* factoryModule = nullptr;
            CreateInterfaceFn createInterface = nullptr;
            const char* factoryModuleName = nullptr;
            for (const char* moduleName : {"libtier0.so", "libengine2.so"}) {
                void* module = dlopen(moduleName, RTLD_NOW | RTLD_NOLOAD);
                auto factory = module ? reinterpret_cast<CreateInterfaceFn>(dlsym(module, "CreateInterface")) : nullptr;
                if (factory) {
                    factoryModule = module;
                    createInterface = factory;
                    factoryModuleName = moduleName;
                    break;
                }
                if (module) dlclose(module);
            }
            if (!createInterface) {
                im.say("FAILED: CreateInterface was not found in loaded libtier0.so or libengine2.so");
            } else {
                im.say(std::string("CreateInterface found in ") + factoryModuleName + "; requesting VEngineCvar007");
                im.cvar = createInterface("VEngineCvar007", nullptr);
                if (!im.cvar) {
                    im.say(std::string("FAILED: ") + factoryModuleName + " does not provide VEngineCvar007");
                } else if (!readable(im.cvar, sizeof(void*))) {
                    im.say("FAILED: VEngineCvar007 returned an object outside readable process memory");
                } else {
                    std::memcpy(&im.vtable, im.cvar, sizeof(im.vtable));
                    if (!readable(im.vtable, sizeof(void*) * 42)) {
                        im.say("FAILED: interface vtable is not readable");
                    } else if (!executableInKnownModule(im.vtable[slotFindConVar]) ||
                               !executableInKnownModule(im.vtable[slotGetConVarData])) {
                        im.say("FAILED: expected virtual methods are not executable functions in CS2 engine modules");
                    } else {
                        im.say("vtable slots 11 and 41 passed module and execute permission checks");
                        im.detectFindAbi(probes[0].data());
                        std::uint64_t ref = 0;
                        if (im.find(missingName, ref)) {
                            im.say("FAILED: missing-name probe unexpectedly resolved");
                        } else {
                            int matches = 0;
                            bool mismatch = false;
                            for (auto name : probes) {
                                if (!im.find(name.data(), ref)) {
                                    im.say("  " + std::string(name) + ": not found");
                                    continue;
                                }
                                std::int16_t type = -1;
                                const auto* record = im.data(ref);
                                if (!im.valid(record, std::string(name), type)) {
                                    im.say("FAILED: data layout check failed for " + std::string(name));
                                    mismatch = true;
                                    break;
                                }
                                im.say("  " + std::string(name) + ": found, type " + std::to_string(type));
                                if (name == "fps_max" && type == 7) {
                                    float at88 = 0.0f;
                                    float at96 = 0.0f;
                                    float defaultValue = 0.0f;
                                    const unsigned char* bytes = static_cast<const unsigned char*>(record);
                                    const unsigned char* defaultPointer = nullptr;
                                    std::memcpy(&at88, bytes + 88, sizeof(at88));
                                    std::memcpy(&at96, bytes + 96, sizeof(at96));
                                    std::memcpy(&defaultPointer, bytes + offDefault, sizeof(defaultPointer));
                                    if (defaultPointer) std::memcpy(&defaultValue, defaultPointer, sizeof(defaultValue));
                                    im.say("fps_max value candidates: offset 88=" + std::to_string(at88) +
                                           ", offset 96=" + std::to_string(at96) +
                                           ", default=" + std::to_string(defaultValue));
                                    im.say("Linux live value offset selected: " + std::to_string(currentValueOffset));
                                }
                                ++matches;
                            }
                            if (!mismatch && matches >= 3) {
                                im.say("RESULT: Linux engine interface and convar records calibrated; read-only access enabled");
                                ok = true;
                            } else if (!mismatch) {
                                im.say("FAILED: fewer than three known convars matched the expected record layout");
                            }
                        }
                    }
                }
            }
            if (factoryModule) dlclose(factoryModule);
        }
    } catch (...) {
        im.say("FAILED: exception during interface discovery");
    }
    im.report.close();
    im.ready.store(ok);
    return ok;
}

ConsoleStatus EngineConsole::read(const std::string& name, bool wantDefault, ConsoleValue& out) {
    Impl& im = *impl;
    if (!im.ready.load()) return ConsoleStatus::unavailable;
    std::lock_guard<std::mutex> lock(im.mutex);
    std::uint64_t ref = 0;
    if (!im.find(name.c_str(), ref)) return ConsoleStatus::notFound;
    const unsigned char* record = im.data(ref);
    std::int16_t type = -1;
    if (!im.valid(record, name, type)) return ConsoleStatus::unavailable;
    const unsigned char* source = record + currentValueOffset;
    if (wantDefault) {
        std::memcpy(&source, record + offDefault, sizeof(source));
        if (!source) return ConsoleStatus::unsupportedType;
    }
    return convert(type, source, out);
}

}  // namespace modloader
