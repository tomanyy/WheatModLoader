#include "mod_host.hpp"
#include "engineConsole.hpp"
#include "frameHook.hpp"

#include <SDL.h>
#include <imgui.h>
#include <backends/imgui_impl_sdl2.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace modloader;

void setImguiFrameActive(bool active);

namespace {
std::atomic<bool> stopping{false};
std::atomic<ModHost*> activeHost{nullptr};
std::thread worker;
std::recursive_mutex hostMutex;
std::mutex loggerMutex;
int proxyModuleAnchor = 0;
std::atomic<bool> imguiPlatformReady{false};
std::atomic<bool> imguiRendererReady{false};
std::atomic<SDL_Window*> imguiWindow{nullptr};
std::atomic<float> imguiDisplayWidth{0.0f};
std::atomic<float> imguiDisplayHeight{0.0f};
std::atomic<const char*> imguiStatus{nullptr};

void wheatPreload();

void dispatchFrame(double dt, std::uint64_t frame) {
    std::lock_guard<std::recursive_mutex> lock(hostMutex);
    if (imguiRendererReady.load(std::memory_order_acquire) && ImGui::GetCurrentContext()) {
        if (imguiPlatformReady.load(std::memory_order_acquire)) {
            ImGui_ImplSDL2_NewFrame();
        } else {
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(imguiDisplayWidth.load(std::memory_order_relaxed),
                                    imguiDisplayHeight.load(std::memory_order_relaxed));
            io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
            io.DeltaTime = static_cast<float>(dt > 0.0 ? dt : 1.0 / 60.0);
        }
        ImGui::NewFrame();
        setImguiFrameActive(true);
    }
    try {
        if (ModHost* host = activeHost.load(std::memory_order_acquire))
            host->frame(dt, static_cast<std::int64_t>(frame));
    } catch (...) {
    }
    if (imguiRendererReady.load(std::memory_order_acquire) && ImGui::GetCurrentContext()) {
        setImguiFrameActive(false);
        ImGui::Render();
    }
}

void initializeImguiPlatform(SDL_Window* window) {
    if (!window || imguiPlatformReady.load(std::memory_order_acquire)) return;
    if (!ImGui::GetCurrentContext()) ImGui::CreateContext();
    if (ImGui_ImplSDL2_InitForVulkan(window)) {
        imguiWindow.store(window, std::memory_order_release);
        imguiPlatformReady.store(true, std::memory_order_release);
    }
}

bool processIsCs2() {
    std::error_code ec;
    const std::string name = fs::read_symlink("/proc/self/exe", ec).filename().string();
    return !ec && (name == "cs2" || name == "cs2_linux64");
}

bool hasInsecureArgument() {
    std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
    std::string argument;
    while (std::getline(cmdline, argument, '\0')) {
        if (argument == "-secure") return false;
        if (argument == "-insecure") return true;
    }
    return false;
}

void runHost() {
    try {
        std::error_code ec;
        Dl_info loaderInfo{};
        if (dladdr(reinterpret_cast<void*>(runHost), &loaderInfo) == 0 || !loaderInfo.dli_fname) return;
        const fs::path loaderPath = fs::absolute(loaderInfo.dli_fname, ec);
        if (ec) return;
        const fs::path wheatDir = loaderPath.parent_path() / "wheat";
        fs::create_directories(wheatDir / "mods", ec);

        std::ofstream logFile(wheatDir / "loader.log", std::ios::out | std::ios::trunc);
        const auto started = std::chrono::steady_clock::now();
        LogFn logger = [&logFile, started](std::string_view mod, LogLevel level, std::string_view message) {
            std::lock_guard<std::mutex> lock(loggerMutex);
            if (!logFile) return;
            static constexpr const char* labels[] = {"INFO", "WARN", "ERROR"};
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            logFile << '[' << elapsed << "] [" << mod << "] " << labels[static_cast<int>(level)] << ' ' << message << '\n';
            logFile.flush();
        };
        logger("host", LogLevel::Info, "Wheat Linux preload started (cs2, -insecure confirmed)");
        logger("host", LogLevel::Info, "Linux ImGui renderer diagnostics build active");
        logger("host", LogLevel::Info, "Linux frame callbacks use the timer until the opt-in Vulkan layer reports a frame");

        auto engineConsole = std::make_shared<EngineConsole>();
        ModHostConfig config;
        config.console = engineConsole;
        config.mods_dir = wheatDir / "mods";
        config.limits.event_wall_ms = 20;
        config.limits.event_instructions = 200'000;
        config.log = logger;
        const fs::path allowFile = wheatDir / "apiAllowList.txt";
        if (fs::exists(allowFile, ec)) {
            config.enforce_api_allow_list = true;
            std::ifstream allowInput(allowFile);
            std::string api;
            while (std::getline(allowInput, api)) {
                if (!api.empty() && api.back() == '\r') api.pop_back();
                if (!api.empty() && api.size() <= 64 &&
                    api.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._") == std::string::npos)
                    config.api_allow_list.insert(api);
            }
        }
        const bool consoleApiEnabled = config.enforce_api_allow_list &&
            (config.api_allow_list.contains("console.get") || config.api_allow_list.contains("console.default") ||
             config.api_allow_list.contains("console.allowed") || config.api_allow_list.contains("console.available"));

        ModHost host(std::move(config));
        activeHost.store(&host, std::memory_order_release);
        std::string frameHookError;
        const bool frameHookArmed = frameHook::install(dispatchFrame, frameHookError);
        logger("host", frameHookArmed ? LogLevel::Info : LogLevel::Warn, frameHookError);
        bool modsLoaded = false;
        const auto loadMods = [&] {
            if (modsLoaded) return;
            std::lock_guard<std::recursive_mutex> lock(hostMutex);
            const std::size_t loaded = host.load_all();
            logger("host", LogLevel::Info, std::to_string(loaded) + " mod(s) running");
            host.dispatch("mods_loaded");
            modsLoaded = true;
        };

        auto previous = std::chrono::steady_clock::now();
        std::uint64_t frame = 0;
        unsigned refresh = 0;
        unsigned consoleWait = 0;
        bool consoleInitialized = !consoleApiEnabled;
        bool modsLoadScheduled = false;
        bool renderFramesLogged = false;
        bool imguiReadyReported = false;
        const char* imguiStatusReported = nullptr;
        std::chrono::steady_clock::time_point modsLoadTime{};
        if (!consoleApiEnabled) {
            logger("host", LogLevel::Info, "console API blocked by installation API Allow List");
        }
        while (!stopping.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!consoleInitialized && ++consoleWait % 10 == 0) {
                void* engine = dlopen("libengine2.so", RTLD_NOW | RTLD_NOLOAD);
                if (engine) {
                    const bool enabled = engineConsole->initialize(wheatDir);
                    if (enabled) {
                        logger("host", LogLevel::Info, "Linux console API enabled (read-only); details in consoleReport.txt");
                        consoleInitialized = true;
                    } else if (consoleWait >= 1800) {
                        logger("host", LogLevel::Warn, "Linux console API stayed unavailable after 180 seconds; see consoleReport.txt");
                        consoleInitialized = true;
                    } else if (consoleWait % 100 == 0) {
                        logger("host", LogLevel::Info, "Linux console API is waiting for CS2 convars to register; retrying calibration");
                    }
                    dlclose(engine);
                } else if (consoleWait >= 1800) {
                    logger("host", LogLevel::Warn, "Linux engine console interface did not load within 180 seconds");
                    consoleInitialized = true;
                }
            }
            const auto now = std::chrono::steady_clock::now();
            if (!renderFramesLogged && frameHook::framesSeen() > 0) {
                renderFramesLogged = true;
                logger("host", LogLevel::Info, "Vulkan layer frames detected; mod callbacks moved to the render thread");
            }
            const bool guiReady = imguiRendererReady.load(std::memory_order_acquire);
            if (guiReady != imguiReadyReported) {
                imguiReadyReported = guiReady;
                logger("host", guiReady ? LogLevel::Info : LogLevel::Warn,
                       guiReady ? "Linux ImGui Vulkan renderer ready" : "Linux ImGui Vulkan renderer unavailable");
            }
            const char* guiStatus = imguiStatus.load(std::memory_order_acquire);
            if (guiStatus && guiStatus != imguiStatusReported) {
                imguiStatusReported = guiStatus;
                const LogLevel statusLevel = std::string(guiStatus).starts_with("Linux ImGui Vulkan renderer initialized")
                    ? LogLevel::Info : LogLevel::Warn;
                logger("host", statusLevel, guiStatus);
            }
            if (consoleInitialized && !modsLoaded) {
                if (!modsLoadScheduled) {
                    modsLoadTime = now + std::chrono::seconds(5);
                    modsLoadScheduled = true;
                    logger("host", LogLevel::Info, "waiting 5 seconds for CS2 startup settings before starting mods");
                } else if (now >= modsLoadTime) {
                    loadMods();
                }
            }
            const double dt = std::chrono::duration<double>(now - previous).count();
            previous = now;
            if (!frameHookArmed || frameHook::framesSeen() == 0) {
                dispatchFrame(dt, ++frame);
            }
            if (modsLoaded && ++refresh == 10) {
                std::lock_guard<std::recursive_mutex> lock(hostMutex);
                host.refresh();
                refresh = 0;
            }
        }
        frameHook::stop();
        activeHost.store(nullptr, std::memory_order_release);
        logger("host", LogLevel::Info, "Wheat Linux preload stopping");
    } catch (...) {
    }
}

__attribute__((constructor)) void wheatPreload() {
    Dl_info loaderInfo{};
    if (dladdr(&proxyModuleAnchor, &loaderInfo) == 0 || !loaderInfo.dli_fname) return;
    const std::string imageName = fs::path(loaderInfo.dli_fname).filename().string();
    if (imageName != "winmm.dll" && imageName != "wheatLinux.dll") return;
    if (!processIsCs2() || !hasInsecureArgument()) return;
    const char* enableVulkanLayer = std::getenv("WHEAT_ENABLE_VULKAN_LAYER");
    if (!enableVulkanLayer || std::string(enableVulkanLayer) != "1")
        setenv("WHEAT_DISABLE_VULKAN_LAYER", "1", 1);
    else
        unsetenv("WHEAT_DISABLE_VULKAN_LAYER");
    stopping.store(false, std::memory_order_relaxed);
    worker = std::thread(runHost);
}

__attribute__((destructor)) void wheatUnload() {
    stopping.store(true, std::memory_order_relaxed);
    if (worker.joinable()) worker.join();
}
}  // namespace

extern "C" __attribute__((visibility("default"))) void
wheatDispatchVulkanFrame(double dt, std::uint64_t frame) {
    frameHook::dispatchFromLayer(dt, frame);
}

extern "C" __attribute__((visibility("default"))) ImDrawData*
wheatGetImGuiDrawData() {
    return ImGui::GetCurrentContext() ? ImGui::GetDrawData() : nullptr;
}

extern "C" __attribute__((visibility("default"))) void
wheatSetImGuiRendererReady(bool ready) {
    imguiRendererReady.store(ready, std::memory_order_release);
}

extern "C" __attribute__((visibility("default"))) void
wheatSetImGuiRendererStatus(const char* status) {
    imguiStatus.store(status, std::memory_order_release);
}

extern "C" __attribute__((visibility("default"))) void
wheatEnsureImGuiContext(float width, float height) {
    std::lock_guard<std::recursive_mutex> lock(hostMutex);
    if (!ImGui::GetCurrentContext()) ImGui::CreateContext();
    imguiDisplayWidth.store(width, std::memory_order_relaxed);
    imguiDisplayHeight.store(height, std::memory_order_relaxed);
}

extern "C" __attribute__((visibility("default"))) void wheatLockImGui() {
    hostMutex.lock();
}

extern "C" __attribute__((visibility("default"))) void wheatUnlockImGui() {
    hostMutex.unlock();
}

extern "C" __attribute__((visibility("default"))) SDL_Window*
SDL_CreateWindow(const char* title, int x, int y, int width, int height, Uint32 flags) {
    using CreateWindow = SDL_Window* (*)(const char*, int, int, int, int, Uint32);
    static auto real = reinterpret_cast<CreateWindow>(dlsym(RTLD_NEXT, "SDL_CreateWindow"));
    if (!real) return nullptr;
    SDL_Window* window = real(title, x, y, width, height, flags);
    initializeImguiPlatform(window);
    return window;
}

extern "C" __attribute__((visibility("default"))) int
SDL_PollEvent(SDL_Event* event) {
    using PollEvent = int (*)(SDL_Event*);
    static auto real = reinterpret_cast<PollEvent>(dlsym(RTLD_NEXT, "SDL_PollEvent"));
    if (!real) return 0;
    if (!event) return real(nullptr);
    int result = 0;
    while ((result = real(event)) != 0) {
        if (!imguiPlatformReady.load(std::memory_order_acquire) || !imguiRendererReady.load(std::memory_order_acquire)) return result;
        std::lock_guard<std::recursive_mutex> lock(hostMutex);
        ImGui_ImplSDL2_ProcessEvent(event);
        const ImGuiIO& io = ImGui::GetIO();
        const bool mouseEvent = event->type == SDL_MOUSEMOTION || event->type == SDL_MOUSEBUTTONDOWN ||
                                event->type == SDL_MOUSEBUTTONUP || event->type == SDL_MOUSEWHEEL;
        const bool keyboardEvent = event->type == SDL_KEYDOWN || event->type == SDL_KEYUP ||
                                   event->type == SDL_TEXTINPUT || event->type == SDL_TEXTEDITING;
        if ((mouseEvent && io.WantCaptureMouse) || (keyboardEvent && io.WantCaptureKeyboard)) continue;
        return result;
    }
    return result;
}

extern "C" __attribute__((visibility("default"))) void
SDL_DestroyWindow(SDL_Window* window) {
    using DestroyWindow = void (*)(SDL_Window*);
    static auto real = reinterpret_cast<DestroyWindow>(dlsym(RTLD_NEXT, "SDL_DestroyWindow"));
    if (!real) return;
    if (window == imguiWindow.load(std::memory_order_acquire)) {
        std::lock_guard<std::recursive_mutex> lock(hostMutex);
        if (imguiPlatformReady.exchange(false, std::memory_order_acq_rel)) ImGui_ImplSDL2_Shutdown();
        imguiWindow.store(nullptr, std::memory_order_release);
    }
    real(window);
}
