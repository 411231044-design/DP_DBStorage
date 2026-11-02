#include "worker.hpp"

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <format>
#include <fstream>
#include <thread>
#include <unordered_map>

#include "schema.hpp"
#include "sqlite.hpp"
#include "store.hpp"
#include "validate.hpp"
#include "db_mariadb.hpp"

namespace dpdb {

namespace {

// ── утилиты путей (порт DP_Storage: ExeDir/CmdArg/JoinPath) ──────────────────

std::string exe_dir() {
    char buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n == MAX_PATH)
        return {};
    std::string p(buf, n);
    if (const auto s = p.find_last_of("\\/"); s != std::string::npos)
        p.resize(s);
    return p;
}

std::string join_path(std::string a, const std::string& b) {
    if (a.empty())
        return b;
    if (a.back() != '\\' && a.back() != '/')
        a += '\\';
    return a + b;
}

// Значение «-ключ=значение» из командной строки (стандарт DayZ-сервера).
std::string cmd_arg(const std::string& key) {
    const std::string dash = "-" + key + "=";
    LPSTR cmd = GetCommandLineA();
    for (char* p = cmd; *p;) {
        while (*p == ' ' || *p == '\t')
            ++p;
        char* tok = p;
        while (*p && *p != ' ' && *p != '\t')
            ++p;
        if (p > tok) {
            const std::string_view sv(tok, p - tok);
            if (sv.starts_with(dash))
                return std::string(sv.substr(dash.size()));
        }
    }
    return {};
}

// Значение «key = value» из serverDZ.cfg: первое вхождение, кавычки снимаются
// (quote != 0), иначе — до пробела/;/комментария.
std::string cfg_value(const std::string& cfg_path, const std::string& key,
                      char quote) {
    std::ifstream f(cfg_path);
    if (!f)
        return {};
    std::string line;
    while (std::getline(f, line)) {
        const auto pos = line.find(key);
        if (pos == std::string::npos)
            continue;
        const auto eq = line.find('=', pos);
        if (eq == std::string::npos)
            continue;
        std::string v = line.substr(eq + 1);
        if (quote) {
            const auto q1 = v.find(quote);
            if (q1 != std::string::npos) {
                const auto q2 = v.find(quote, q1 + 1);
                if (q2 != std::string::npos)
                    return v.substr(q1 + 1, q2 - q1 - 1);
            }
        }
        if (!v.empty()) {
            const auto start = v.find_first_not_of(" \t");
            if (start == std::string::npos)
                return {};
            v = v.substr(start);
            const auto sp = v.find_first_of(" ;\t\r/");
            if (sp != std::string::npos)
                v.resize(sp);
            return v;
        }
    }
    return {};
}

// template="<миссия>" из serverDZ.cfg (первое вхождение, кавычки снимаются).
std::string template_from_cfg(const std::string& cfg_path) {
    return cfg_value(cfg_path, "template", '"');
}

// instanceId = N (1..99): persistence живёт в storage_<N>, не всегда storage_1.
int instance_id_from_cfg(const std::string& cfg_path) {
    const std::string v = cfg_value(cfg_path, "instanceId", 0);
    if (v.empty())
        return 1;
    int id = 1;
    const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), id);
    if (ec != std::errc() || id < 1 || id > 99)
        return 1;
    return id;
}

// Текущее время «2026-11-02 13:44:05».
std::string now() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return std::format("{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}", st.wYear,
                       st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

std::mutex g_log_mx;

// FNV-1a 64 — детектор изменений теневых BLOB (не криптография).
std::uint64_t fnv1a64(const std::uint8_t* p, std::size_t n) {
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

void set_phase(const char* phase) {
    auto& st = worker_state();
    std::lock_guard<std::mutex> lk(st.mx);
    st.phase = phase;
}

// ── теневые копии: players.db + storage_1/data/*.bin → shadow_* ──────────────

// Существующие тени (uid → hash), чтобы не гонять мегабайты без изменений.
std::unordered_map<std::string, std::uint64_t> shadow_hashes(DbClient& db,
                                                             bool files) {
    std::unordered_map<std::string, std::uint64_t> out;
    std::vector<DbRow> rows;
    const char* table = files ? "shadow_files" : "shadow_players";
    const char* key = files ? "name" : "uid";
    if (db.query(std::format("SELECT {}, hash FROM {}", key, table), rows))
        for (const auto& r : rows)
            if (r.cols.size() >= 2) {
                const auto* name = std::get_if<std::string>(&r.cols[0]);
                const auto* hash = std::get_if<std::int64_t>(&r.cols[1]);
                if (name && hash)
                    out.emplace(*name, static_cast<std::uint64_t>(*hash));
            }
    return out;
}

void step_shadows(DbClient& db, const Paths& paths) {
    // players.db: UID → Data (сериализация движка) — если движок её менял
    if (!paths.players_db.empty()) {
        sqlite::Db sdb;
        const std::string open_err = sdb.open(paths.players_db, sqlite::kOpenReadonly);
        if (open_err.empty()) {
            std::vector<std::vector<sqlite::Cell>> rows;
            if (sdb.query("SELECT UID, Data FROM Players", rows)) {
                auto known = shadow_hashes(db, false);
                for (const auto& row : rows) {
                    if (row.size() < 2 || row[0].text.empty())
                        continue;
                    const auto& blob = row[1].blob;
                    const std::uint64_t h =
                        fnv1a64(blob.data(), blob.size());
                    if (const auto it = known.find(row[0].text);
                        it != known.end() && it->second == h)
                        continue;
                    if (!db.exec(std::format(
                            "REPLACE INTO shadow_players(uid, data, bytes, hash,"
                            " updated) VALUES ({}, {}, {}, {}, NOW())",
                            sql_quote(row[0].text),
                            hex_literal(blob.data(), blob.size()),
                            blob.size(), static_cast<std::int64_t>(h))))
                        return;   // СУБД умерла — на следующий цикл
                }
            } else {
                log_line("тень players.db: query: " + sdb.last_error());
            }
        } else {
            log_line("тень players.db: " + open_err);
        }
    }
    // storage_<id>/data/*.bin — байты файлов
    if (!paths.data_dir.empty()) {
        const std::string pattern = join_path(paths.data_dir, "*.bin");
        WIN32_FIND_DATAA fd{};
        HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) {
            log_line("тень слоёв: нет файлов по " + pattern);
            return;
        }
        auto known = shadow_hashes(db, true);
        int sent = 0, skipped = 0;
        std::string first_err;
        do {
            std::ifstream f(join_path(paths.data_dir, fd.cFileName),
                            std::ios::binary);
            if (!f)
                continue;
            Blob bytes((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
            const std::uint64_t hashv = fnv1a64(bytes.data(), bytes.size());
            if (const auto it = known.find(fd.cFileName);
                it != known.end() && it->second == hashv) {
                ++skipped;
                continue;
            }
            if (!db.exec(std::format(
                    "REPLACE INTO shadow_files(name, data, bytes, hash, updated)"
                    " VALUES ({}, {}, {}, {}, NOW())",
                    sql_quote(fd.cFileName),
                    hex_literal(bytes.data(), bytes.size()), bytes.size(),
                    static_cast<std::int64_t>(hashv)))) {
                if (first_err.empty())
                    first_err = std::string(fd.cFileName) + ": " + db.error();
                FindClose(h);
                log_line("тень слоёв: СУБД: " + first_err);
                return;
            }
            ++sent;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        if (sent || first_err.empty())
            log_line(std::format("тень слоёв: отправлено {}, без изменений {}",
                                 sent, skipped));
    }
}

}  // namespace

Paths resolve_paths() {
    Paths p;
    p.exe_dir = exe_dir();
    const std::string prof = cmd_arg("profiles");
    p.profiles =
        prof.empty() ? join_path(p.exe_dir, "profiles") : join_path(p.exe_dir, prof);
    p.log_file = join_path(p.profiles, "dp_dbstorage.log");

    const std::string cfg = cmd_arg("config");
    const std::string tmpl = template_from_cfg(
        cfg.empty() ? std::string() : join_path(p.exe_dir, cfg));
    if (tmpl.empty()) {
        return p;   // миссию не нашли — игроки/слои недоступны, журнал работает
    }
    p.mission = join_path(join_path(p.exe_dir, "mpmissions"), tmpl);
    const int inst = instance_id_from_cfg(join_path(p.exe_dir, cfg));
    p.storage = join_path(p.mission, "storage_" + std::to_string(inst));
    p.data_dir = join_path(p.storage, "data");
    p.players_db = join_path(p.storage, "players.db");
    p.resolved = true;
    return p;
}

void log_line(const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_log_mx);
    std::ofstream f(worker_state().paths.log_file, std::ios::app);
    if (f)
        f << now() << " " << msg << "\n";
}

WorkerState& worker_state() {
    static WorkerState st;
    return st;
}

void worker_main(DbConfig cfg, Paths paths) {
    {
        auto& st = worker_state();
        std::lock_guard<std::mutex> lk(st.mx);
        st.cfg = cfg;
        st.paths = paths;
    }
    auto client = make_mariadb_client(cfg);
    std::string report;
    for (;;) {
        if (!client->connected()) {
            set_phase("connect");
            if (!client->reconnect()) {
                auto& st = worker_state();
                std::lock_guard<std::mutex> lk(st.mx);
                st.last_error = client->error();
                std::this_thread::sleep_for(std::chrono::seconds(5));
                continue;
            }
            auto& st = worker_state();
            std::lock_guard<std::mutex> lk(st.mx);
            st.server_info = client->server_info();
        }

        set_phase("schema");
        std::string mig_log;
        if (!apply_migrations(*client, mig_log)) {
            auto& st = worker_state();
            std::lock_guard<std::mutex> lk(st.mx);
            st.last_error = mig_log + " (" + client->error() + ")";
            log_line("схема: " + st.last_error);
            client->reconnect();
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }
        if (!mig_log.empty())
            log_line("схема: " + mig_log);

        set_phase("ready");
        store().drain(*client, report);
        step_shadows(*client, paths);
        client->exec(
            "REPLACE INTO server_meta(k, v) VALUES('heartbeat', NOW())");
        {
            auto& st = worker_state();
            std::lock_guard<std::mutex> lk(st.mx);
            st.last_report = report;
            st.last_error = client->error();
            ++st.cycles;
        }
        std::this_thread::sleep_for(
            std::chrono::seconds(std::max(1, cfg.sync_period_sec)));
    }
}

void start_worker(DbConfig cfg, Paths paths) {
    std::thread(worker_main, std::move(cfg), std::move(paths)).detach();
}

}  // namespace dpdb
