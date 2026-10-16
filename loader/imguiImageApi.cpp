#include "imguiImageApi.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <lua.hpp>
#include <png.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace {

namespace fs = std::filesystem;
constexpr std::size_t maxEncodedImageBytes = 16 * 1024 * 1024;
constexpr std::size_t maxDecodedImageBytes = 16 * 1024 * 1024;
constexpr std::size_t maxCachedImageBytes = 64 * 1024 * 1024;
constexpr std::size_t maxCachedImages = 64;

struct CachedImage {
    std::unique_ptr<ImTextureData> texture;
    std::size_t bytes = 0;
};

std::unordered_map<std::string, CachedImage>& imageCache() {
    static auto* cache = new std::unordered_map<std::string, CachedImage>();
    return *cache;
}

std::size_t& cachedImageBytes() {
    static auto* bytes = new std::size_t(0);
    return *bytes;
}

ImTextureData* loadImage(const fs::path& modRoot, const std::string& relativePath) {
    if (relativePath.empty() || relativePath.size() > 512) return nullptr;
    const fs::path relative(relativePath);
    if (relative.is_absolute()) return nullptr;

    std::error_code ec;
    const fs::path root = fs::weakly_canonical(modRoot, ec);
    if (ec) return nullptr;
    const fs::path file = fs::weakly_canonical(root / relative, ec);
    if (ec || !fs::is_regular_file(file, ec)) return nullptr;
    const fs::path inside = file.lexically_relative(root);
    if (inside.empty() || *inside.begin() == "..") return nullptr;

    const std::string key = file.string();
    auto& cache = imageCache();
    if (const auto found = cache.find(key); found != cache.end()) return found->second.texture.get();
    if (cache.size() >= maxCachedImages || cachedImageBytes() >= maxCachedImageBytes) return nullptr;

    const std::uintmax_t fileSize = fs::file_size(file, ec);
    if (ec || fileSize == 0 || fileSize > maxEncodedImageBytes) return nullptr;
    std::vector<unsigned char> encoded(static_cast<std::size_t>(fileSize));
    std::ifstream input(file, std::ios::binary);
    if (!input || !input.read(reinterpret_cast<char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()))) return nullptr;

    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, encoded.data(), encoded.size())) return nullptr;
    image.format = PNG_FORMAT_RGBA;
    const std::size_t decodedSize = PNG_IMAGE_SIZE(image);
    if (image.width == 0 || image.height == 0 || image.width > 4096 || image.height > 4096 ||
        decodedSize > maxDecodedImageBytes || decodedSize > maxCachedImageBytes - cachedImageBytes()) {
        png_image_free(&image);
        return nullptr;
    }
    std::vector<unsigned char> pixels(decodedSize);
    if (!png_image_finish_read(&image, nullptr, pixels.data(), 0, nullptr)) {
        png_image_free(&image);
        return nullptr;
    }
    const int width = static_cast<int>(image.width);
    const int height = static_cast<int>(image.height);
    png_image_free(&image);

    auto texture = std::make_unique<ImTextureData>();
    texture->Create(ImTextureFormat_RGBA32, width, height);
    std::memcpy(texture->Pixels, pixels.data(), pixels.size());
    texture->UseColors = true;
    ImTextureData* result = texture.get();
    cache.emplace(key, CachedImage{std::move(texture), pixels.size()});
    cachedImageBytes() += pixels.size();
    ImGui::RegisterUserTexture(result);
    return result;
}

int image(lua_State* state) {
    const int arguments = lua_gettop(state);
    if (arguments < 1 || arguments > 3 || lua_type(state, 1) != LUA_TSTRING ||
        (arguments >= 2 && lua_type(state, 2) != LUA_TNUMBER) ||
        (arguments >= 3 && lua_type(state, 3) != LUA_TNUMBER)) {
        return luaL_error(state, "imgui.image expects a PNG path and optional width and height");
    }
    if (!imguiFrameActive() || !ImGui::GetCurrentContext()) return 0;

    std::size_t pathLength = 0;
    const char* path = lua_tolstring(state, 1, &pathLength);
    const char* rootValue = lua_tostring(state, lua_upvalueindex(1));
    if (!path || !rootValue || pathLength == 0 || pathLength > 512) return 0;
    const bool hasWidth = arguments >= 2;
    const bool hasHeight = arguments >= 3;
    const double requestedWidth = hasWidth ? lua_tonumber(state, 2) : 0.0;
    const double requestedHeight = hasHeight ? lua_tonumber(state, 3) : 0.0;
    if ((hasWidth && (!std::isfinite(requestedWidth) || requestedWidth <= 0.0 || requestedWidth > 4096.0)) ||
        (hasHeight && (!std::isfinite(requestedHeight) || requestedHeight <= 0.0 || requestedHeight > 4096.0))) return 0;

    try {
        ImTextureData* texture = loadImage(fs::path(rootValue), std::string(path, pathLength));
        if (!texture) return 0;
        float width = hasWidth ? static_cast<float>(requestedWidth) : static_cast<float>(texture->Width);
        float height = hasHeight ? static_cast<float>(requestedHeight) : static_cast<float>(texture->Height);
        if (hasWidth && !hasHeight) height = width * static_cast<float>(texture->Height) / static_cast<float>(texture->Width);
        if (!hasWidth && hasHeight) width = height * static_cast<float>(texture->Width) / static_cast<float>(texture->Height);
        ImGui::Image(texture->GetTexRef(), ImVec2(width, height));
    } catch (...) {
        return 0;
    }
    return 0;
}

}  // namespace

void addImguiImageApi(lua_State* state, const std::filesystem::path& modRoot,
                      const std::set<std::string>& allowed, bool enforce) {
    if (enforce && !allowed.contains("imgui.image")) return;
    const std::string root = modRoot.string();
    lua_pushlstring(state, root.data(), root.size());
    lua_pushcclosure(state, image, 1);
    lua_setfield(state, -2, "image");
}
