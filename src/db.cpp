#include "db.hpp"

#include <charconv>
#include <cmath>
#include <format>

#include "validate.hpp"

namespace dpdb {

// ── Литералы ─────────────────────────────────────────────────────────────────

std::string sql_literal(const Val& v) {
    if (v.index() == 0)
        return "NULL";
    if (const auto* i = std::get_if<std::int64_t>(&v))
        return std::to_string(*i);
    if (const auto* d = std::get_if<double>(&v))
        return std::isfinite(*d) ? std::format("{}", *d) : "NULL";
    if (const auto* s = std::get_if<std::string>(&v))
        return std::format("'{}'", sql_quote(*s));
    if (const auto* b = std::get_if<Blob>(&v))
        return hex_literal(b->data(), b->size());
    return "NULL";
}

// ── Конфиг ───────────────────────────────────────────────────────────────────

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);
    return s;
}

bool parse_int(std::string_view s, int lo, int hi, int& out) {
    int v = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc() || ptr != s.data() + s.size() || v < lo || v > hi)
        return false;
    out = v;
    return true;
}

}  // namespace

ConfigResult parse_config(std::string_view text) {
    ConfigResult r;
    r.ok = true;
    if (text.starts_with("\xEF\xBB\xBF"))
        text.remove_prefix(3);

    DbConfig& c = r.cfg;
    for (std::size_t pos = 0; pos <= text.size();) {
        std::size_t eol = text.find('\n', pos);
        std::string_view line = text.substr(
            pos, eol == std::string_view::npos ? std::string_view::npos : eol - pos);
        pos = eol == std::string_view::npos ? text.size() + 1 : eol + 1;

        if (const auto hash = line.find('#'); hash != std::string_view::npos)
            line = line.substr(0, hash);
        line = trim(line);
        if (line.empty())
            continue;

        const auto eq = line.find('=');
        if (eq == std::string_view::npos || eq == 0) {
            r.ok = false;
            r.error = std::format("конфиг: строка без «ключ = значение»: «{}»",
                                  line);
            return r;
        }
        const std::string key(trim(line.substr(0, eq)));
        const std::string_view val = trim(line.substr(eq + 1));
        if (val.empty()) {
            r.ok = false;
            r.error = "конфиг: пустое значение у «" + key + "»";
            return r;
        }

        bool known = true;
        if (key == "host") {
            c.host = val;
        } else if (key == "port") {
            known = parse_int(val, 1, 65535, c.port);
        } else if (key == "user") {
            c.user = val;
        } else if (key == "password") {
            c.password = val;
        } else if (key == "database") {
            c.database = val;
        } else if (key == "mode") {
            if (val == "streams" || val == "takeover" || val == "mirror")
                c.mode = val;
            else {
                r.ok = false;
                r.error =
                    "конфиг: mode — streams, takeover или mirror, получено «" +
                    std::string(val) + "»";
                return r;
            }
        } else if (key == "sync_period_sec") {
            known = parse_int(val, 1, 3600, c.sync_period_sec);
        } else if (key == "player_save_sec") {
            known = parse_int(val, 10, 3600, c.player_save_sec);
        } else if (key == "world_save_sec") {
            known = parse_int(val, 30, 3600, c.world_save_sec);
        } else if (key == "events") {
            known = parse_int(val, 0, 1, c.events);
        } else if (key == "event_retain_hours") {
            known = parse_int(val, 0, 24 * 365, c.event_retain_hours);
        } else {
            r.ok = false;
            r.error = "конфиг: неизвестный ключ «" + key + "»";
            return r;
        }
        if (!known) {
            r.ok = false;
            r.error = "конфиг: значение вне диапазона у «" + key + "»: «" +
                      std::string(val) + "»";
            return r;
        }
    }
    return r;
}

}  // namespace dpdb
