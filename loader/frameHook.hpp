#pragma once
#include <cstdint>
#include <string>

namespace frameHook {

using Callback = void (*)(double dt, std::uint64_t frameNumber);

bool install(Callback callback, std::string& error);
void dispatchFromLayer(double dt, std::uint64_t frameNumber);
void stop();
std::uint64_t framesSeen();

}  // namespace frameHook
