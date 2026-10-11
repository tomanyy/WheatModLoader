#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <atomic>
#include <cstdio>

#include "frameHook.hpp"

namespace {

constexpr int presentSlot = 8;
constexpr int present1Slot = 22;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

frameHook::Callback gCallback = nullptr;
PresentFn gOriginalPresent = nullptr;
Present1Fn gOriginalPresent1 = nullptr;
std::atomic<bool> gStopped{false};
std::atomic<void*> gMainSwapChain{nullptr};
std::atomic<long long> gLastTicks{0};
std::atomic<std::uint64_t> gFrames{0};
thread_local int gDepth = 0;

long long ticksPerSecond() {
    static const long long freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    return freq;
}

void fire(void* swapChain) {
    if (gStopped.load(std::memory_order_relaxed) || !gCallback) return;

    LARGE_INTEGER nowTicks;
    QueryPerformanceCounter(&nowTicks);
    const long long now = nowTicks.QuadPart;

    void* mainChain = gMainSwapChain.load();
    if (mainChain != swapChain) {
        const bool stale = mainChain == nullptr || now - gLastTicks.load() > ticksPerSecond();
        if (!stale) return;
        gMainSwapChain.store(swapChain);
        gLastTicks.store(0);
    }

    const long long previous = gLastTicks.exchange(now);
    double dt = previous ? static_cast<double>(now - previous) / static_cast<double>(ticksPerSecond()) : 0.0;
    if (dt > 1.0) dt = 1.0;

    const std::uint64_t number = ++gFrames;
    try {
        gCallback(dt, number);
    } catch (...) {
    }
}

HRESULT STDMETHODCALLTYPE detourPresent(IDXGISwapChain* self, UINT syncInterval, UINT flags) {
    const bool outermost = gDepth++ == 0;
    if (outermost) fire(self);
    const HRESULT hr = gOriginalPresent(self, syncInterval, flags);
    --gDepth;
    return hr;
}

HRESULT STDMETHODCALLTYPE detourPresent1(IDXGISwapChain1* self, UINT syncInterval, UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
    const bool outermost = gDepth++ == 0;
    if (outermost) fire(self);
    const HRESULT hr = gOriginalPresent1(self, syncInterval, flags, params);
    --gDepth;
    return hr;
}

bool patchSlot(void** vtable, int index, void* detour, void** original) {
    void** slot = vtable + index;
    if (*slot == detour) return true;
    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) return false;
    *original = *slot;
    InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(slot), detour);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
    return true;
}

struct Vtables {
    void** base = nullptr;
    void** v1 = nullptr;
};

void setError(std::string& error, const char* what, HRESULT hr) {
    char buffer[160];
    std::snprintf(buffer, sizeof buffer, "%s (0x%08lx)", what, static_cast<unsigned long>(hr));
    error = buffer;
}

bool readVtables(Vtables& out, std::string& error) {
    HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
    if (!d3d11) {
        error = "d3d11.dll is not loaded";
        return false;
    }
    auto create = reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"));
    if (!create) {
        error = "D3D11CreateDeviceAndSwapChain not found";
        return false;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"wheatFrameProbe";
    RegisterClassExW(&wc);
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 16, 16, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) {
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        error = "could not create probe window";
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 1;
    desc.BufferDesc.Width = 16;
    desc.BufferDesc.Height = 16;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    IDXGISwapChain* swapChain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL obtained{};
    const HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 3, D3D11_SDK_VERSION, &desc, &swapChain, &device, &obtained, &context);

    bool ok = SUCCEEDED(hr) && swapChain;
    if (ok) {
        out.base = *reinterpret_cast<void***>(swapChain);
        IDXGISwapChain1* swapChain1 = nullptr;
        if (SUCCEEDED(swapChain->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&swapChain1))) && swapChain1) {
            out.v1 = *reinterpret_cast<void***>(swapChain1);
            swapChain1->Release();
        }
    } else {
        setError(error, "probe swapchain creation failed", hr);
    }

    if (swapChain) swapChain->Release();
    if (context) context->Release();
    if (device) device->Release();
    DestroyWindow(window);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

}

namespace frameHook {

bool install(Callback callback, std::string& error) {
    Vtables vtables;
    if (!readVtables(vtables, error)) return false;

    gCallback = callback;
    if (!patchSlot(vtables.base, presentSlot, reinterpret_cast<void*>(detourPresent), reinterpret_cast<void**>(&gOriginalPresent))) {
        error = "could not patch IDXGISwapChain::Present";
        return false;
    }
    if (vtables.v1 && !patchSlot(vtables.v1, present1Slot, reinterpret_cast<void*>(detourPresent1), reinterpret_cast<void**>(&gOriginalPresent1))) {
        error = "patched Present but not Present1";
    }
    return true;
}

void stop() { gStopped.store(true); }

std::uint64_t framesSeen() { return gFrames.load(); }

}
