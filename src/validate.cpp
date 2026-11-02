#include "validate.hpp"

namespace dpdb {

const std::vector<std::string_view>& layers() {
    // Живые слои мира (сущности в storage_1/data). types/doors/events-файлы —
    // не сущности (бухгалтерия CE), loot — обобщение dynamic+stashes.
    static const std::vector<std::string_view> kLayers = {
        "dynamic", "vehicles", "zombies", "animals", "buildings", "stashes",
        "events"};
    return kLayers;
}

bool valid_uid(std::string_view s) {
    if (s.size() < 5 || s.size() > 64)
        return false;
    for (const char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '=' || c == '_' || c == '-';
        if (!ok)
            return false;
    }
    return true;
}

bool valid_type_name(std::string_view s) {
    if (s.empty() || s.size() > 96)
        return false;
    for (const char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_';
        if (!ok)
            return false;
    }
    return true;
}

bool valid_layer(std::string_view s) {
    for (const auto l : layers())
        if (s == l)
            return true;
    return false;
}

namespace {
bool printable_no_quote(char c) {
    return c >= 0x20 && c < 0x7F && c != '\'' && c != '\\';
}
}  // namespace

bool valid_slot(std::string_view s) {
    if (s.size() > 64)
        return false;
    for (const char c : s)
        if (!printable_no_quote(c))
            return false;
    return true;
}

bool valid_text(std::string_view s, std::size_t max_len) {
    if (s.size() > max_len)
        return false;
    for (const char c : s)
        if (c >= 0 && c < 0x20)   // управляющие — мимо (UTF-8 пропускаем как есть)
            return false;
    return true;
}

std::string sql_quote(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '\'';
    for (const char c : s) {
        if (c == '\'' || c == '\\')
            out += c;   // удвоение: '…' и \ — оба escape-символы MariaDB
        out += c;
    }
    out += '\'';
    return out;
}

std::string hex_literal(const std::uint8_t* p, std::size_t n) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(2 + 2 * n);
    out += "0x";
    for (std::size_t i = 0; i < n; ++i) {
        out += kHex[p[i] >> 4];
        out += kHex[p[i] & 0xF];
    }
    return out;
}

}  // namespace dpdb
