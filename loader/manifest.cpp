#include "manifest.hpp"

#include <charconv>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <set>
#include <string_view>
#include <utility>

namespace fs = std::filesystem;

namespace modloader {
namespace {

constexpr std::size_t maxManifestBytes = 16 * 1024;

class JsonReader {
public:
    explicit JsonReader(std::string_view source) : source(source) {}

    bool readManifest(Manifest& manifest, std::string& error) {
        skipSpace();
        if (!take('{')) return fail(error, "manifest must be a JSON object");
        skipSpace();
        std::set<std::string> seen;
        if (!take('}')) {
            while (true) {
                std::string key;
                if (!readString(key)) return fail(error, "object key must be a valid JSON string");
                if (!seen.insert(key).second) return fail(error, "duplicate field '" + key + "'");
                skipSpace();
                if (!take(':')) return fail(error, "expected ':' after object key");
                skipSpace();
                if (key == "name") {
                    if (!readString(manifest.name)) return fail(error, "'name' must be a string");
                } else if (key == "version") {
                    if (!readString(manifest.version)) return fail(error, "'version' must be a string");
                } else if (key == "description") {
                    if (!readString(manifest.description)) return fail(error, "'description' must be a string");
                } else if (key == "author") {
                    if (!readString(manifest.author)) return fail(error, "'author' must be a string");
                } else if (key == "apiVersion") {
                    if (!readInteger(manifest.apiVersion)) return fail(error, "'apiVersion' must be an integer");
                } else if (!skipValue(0)) {
                    return fail(error, "invalid JSON value for '" + key + "'");
                }
                skipSpace();
                if (take('}')) break;
                if (!take(',')) return fail(error, "expected ',' or '}' in manifest object");
                skipSpace();
            }
        }
        skipSpace();
        if (position != source.size()) return fail(error, "unexpected content after manifest object");
        if (!seen.contains("name")) return fail(error, "manifest is missing 'name'");
        if (!seen.contains("version")) return fail(error, "manifest is missing 'version'");
        if (!seen.contains("apiVersion")) return fail(error, "manifest is missing 'apiVersion'");
        if (manifest.name.empty()) return fail(error, "'name' must not be empty");
        if (manifest.name.size() > 64) return fail(error, "'name' is too long (maximum 64 characters)");
        if (manifest.version.size() > 32) return fail(error, "'version' is too long (maximum 32 characters)");
        if (manifest.description.size() > 200) return fail(error, "'description' is too long (maximum 200 characters)");
        if (manifest.author.size() > 64) return fail(error, "'author' is too long (maximum 64 characters)");
        return true;
    }

private:
    std::string_view source;
    std::size_t position = 0;

    void skipSpace() {
        while (position < source.size() && (source[position] == ' ' || source[position] == '\t' ||
               source[position] == '\r' || source[position] == '\n')) ++position;
    }

    bool take(char expected) {
        if (position >= source.size() || source[position] != expected) return false;
        ++position;
        return true;
    }

    static void appendUtf8(std::string& output, std::uint32_t codepoint) {
        if (codepoint <= 0x7F) output.push_back(static_cast<char>(codepoint));
        else if (codepoint <= 0x7FF) {
            output.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else if (codepoint <= 0xFFFF) {
            output.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else {
            output.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    }

    bool readHex4(std::uint32_t& value) {
        if (source.size() - position < 4) return false;
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = source[position++];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool readString(std::string& output) {
        skipSpace();
        if (!take('"')) return false;
        output.clear();
        while (position < source.size()) {
            const unsigned char c = static_cast<unsigned char>(source[position++]);
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') {
                output.push_back(static_cast<char>(c));
                continue;
            }
            if (position >= source.size()) return false;
            switch (source[position++]) {
                case '"': output.push_back('"'); break;
                case '\\': output.push_back('\\'); break;
                case '/': output.push_back('/'); break;
                case 'b': output.push_back('\b'); break;
                case 'f': output.push_back('\f'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                case 'u': {
                    std::uint32_t codepoint = 0;
                    if (!readHex4(codepoint)) return false;
                    if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                        if (source.size() - position < 6 || source[position] != '\\' || source[position + 1] != 'u') return false;
                        position += 2;
                        std::uint32_t low = 0;
                        if (!readHex4(low) || low < 0xDC00 || low > 0xDFFF) return false;
                        codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                    } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) return false;
                    appendUtf8(output, codepoint);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool readInteger(long long& output) {
        const std::size_t start = position;
        if (position < source.size() && source[position] == '-') ++position;
        if (position >= source.size()) return false;
        if (source[position] == '0') ++position;
        else {
            if (source[position] < '1' || source[position] > '9') return false;
            while (position < source.size() && source[position] >= '0' && source[position] <= '9') ++position;
        }
        if (position < source.size() && (source[position] == '.' || source[position] == 'e' || source[position] == 'E')) return false;
        const auto result = std::from_chars(source.data() + start, source.data() + position, output);
        return result.ec == std::errc{} && result.ptr == source.data() + position;
    }

    bool skipNumber() {
        const std::size_t start = position;
        if (position < source.size() && source[position] == '-') ++position;
        if (position >= source.size()) return false;
        if (source[position] == '0') ++position;
        else {
            if (source[position] < '1' || source[position] > '9') return false;
            while (position < source.size() && source[position] >= '0' && source[position] <= '9') ++position;
        }
        if (position < source.size() && source[position] == '.') {
            ++position;
            const std::size_t fraction = position;
            while (position < source.size() && source[position] >= '0' && source[position] <= '9') ++position;
            if (position == fraction) return false;
        }
        if (position < source.size() && (source[position] == 'e' || source[position] == 'E')) {
            ++position;
            if (position < source.size() && (source[position] == '+' || source[position] == '-')) ++position;
            const std::size_t exponent = position;
            while (position < source.size() && source[position] >= '0' && source[position] <= '9') ++position;
            if (position == exponent) return false;
        }
        return position > start;
    }

    bool skipValue(unsigned depth) {
        if (depth > 32 || position >= source.size()) return false;
        if (source[position] == '"') {
            std::string ignored;
            return readString(ignored);
        }
        if (take('{')) {
            skipSpace();
            if (take('}')) return true;
            while (true) {
                std::string key;
                if (!readString(key)) return false;
                skipSpace();
                if (!take(':')) return false;
                skipSpace();
                if (!skipValue(depth + 1)) return false;
                skipSpace();
                if (take('}')) return true;
                if (!take(',')) return false;
                skipSpace();
            }
        }
        if (take('[')) {
            skipSpace();
            if (take(']')) return true;
            while (true) {
                if (!skipValue(depth + 1)) return false;
                skipSpace();
                if (take(']')) return true;
                if (!take(',')) return false;
                skipSpace();
            }
        }
        for (const std::string_view literal : {"true", "false", "null"}) {
            if (source.substr(position, literal.size()) == literal) {
                position += literal.size();
                return true;
            }
        }
        return skipNumber();
    }

    bool fail(std::string& error, std::string message) {
        error = std::move(message);
        return false;
    }
};

}  // namespace

ManifestStatus readManifest(const fs::path& file, Manifest& out, std::string& error) {
    out = Manifest{};
    error.clear();
    std::error_code ec;
    if (!fs::exists(file, ec)) return ManifestStatus::missing;
    if (!fs::is_regular_file(file, ec)) {
        error = "manifest.json is not a regular file";
        return ManifestStatus::invalid;
    }
    const auto size = fs::file_size(file, ec);
    if (ec || size > maxManifestBytes) {
        error = "manifest.json is unreadable or larger than 16 KB";
        return ManifestStatus::invalid;
    }
    std::ifstream in(file, std::ios::binary);
    std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    JsonReader reader(source);
    if (!reader.readManifest(out, error)) {
        error = "invalid manifest.json: " + error;
        return ManifestStatus::invalid;
    }
    return ManifestStatus::ok;
}

}  // namespace modloader
