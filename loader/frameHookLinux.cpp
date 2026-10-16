#include "frameHook.hpp"

#include <atomic>
#include <thread>

namespace {
std::atomic<frameHook::Callback> callback{nullptr};
std::atomic<bool> stopped{false};
std::atomic<std::uint64_t> frames{0};
std::atomic<unsigned> activeCallbacks{0};
}

namespace frameHook {

bool install(Callback callback, std::string& error) {
    if (!callback) {
        error = "Linux timer callback was empty";
        return false;
    }
    frames.store(0, std::memory_order_relaxed);
    stopped.store(false, std::memory_order_release);
    ::callback.store(callback, std::memory_order_release);
    error = "Linux frame callback bridge ready";
    return true;
}

void dispatchFromLayer(double dt, std::uint64_t frame) {
    frames.store(frame, std::memory_order_relaxed);
    activeCallbacks.fetch_add(1, std::memory_order_acq_rel);
    struct Guard {
        ~Guard() { activeCallbacks.fetch_sub(1, std::memory_order_acq_rel); }
    } guard;
    if (stopped.load(std::memory_order_acquire)) return;
    const auto current = callback.load(std::memory_order_acquire);
    if (current) {
        try { current(dt, frame); } catch (...) {}
    }
}

void stop() {
    stopped.store(true, std::memory_order_release);
    callback.store(nullptr, std::memory_order_release);
    while (activeCallbacks.load(std::memory_order_acquire)) std::this_thread::yield();
}

std::uint64_t framesSeen() { return frames.load(std::memory_order_relaxed); }

}  // namespace frameHook
