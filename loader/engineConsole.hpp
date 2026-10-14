#pragma once
#include "consoleApi.hpp"

#include <filesystem>
#include <memory>

namespace modloader {

class EngineConsole : public ConsoleBackend {
public:
    EngineConsole();
    ~EngineConsole() override;

    bool initialize(const std::filesystem::path& wheatDir);
    bool available() const override;
    ConsoleStatus read(const std::string& name, bool wantDefault, ConsoleValue& out) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace modloader
