#include "boot.hpp"

#include <windows.h>

#include <format>
#include <fstream>

#include "db.hpp"
#include "sqlite.hpp"
#include "worker.hpp"

namespace dpdb::boot {

namespace {

// Группы файлов слоёв в storage_1/data (маппинг DP_Storage): всё, что движок
// восстанавливает при старте. В takeover-режиме это мусор — истина в MariaDB.
const char* kLayerGroups[] = {
    "dynamic_*", "vehicles.*", "zombies.*", "animals.*",
    "doors.*",   "types.*",    "events.*",  "building*",
};

int delete_by_group(const std::string& dir, const char* group) {
    const std::string pattern = dir + "\\" + group;
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    int n = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (DeleteFileA((dir + "\\" + fd.cFileName).c_str()))
            ++n;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

// Убрать persistence движка до его чтения: слои файлами, игроки строками
// (схему players.db не трогаем — движок ждёт валидную пустую базу).
void takeover_wipe(const Paths& paths) {
    if (!paths.resolved) {
        log_line("takeover: миссия не найдена — чистить нечего");
        return;
    }
    int files = 0;
    for (const char* g : kLayerGroups)
        files += delete_by_group(paths.data_dir, g);
    int players = -1;
    sqlite::Db db;
    if (db.open(paths.players_db, sqlite::kOpenReadWrite).empty()) {
        players = db.exec("DELETE FROM Players").empty() ? 1 : 0;
        if (players == 0)
            log_line("takeover: players.db: " + db.last_error());
    }
    log_line(std::format("takeover: удалено файлов слоёв {}, players.db: {}",
                         files, players > 0 ? "чищен" : "не тронут"));
}

DbConfig load_config(const Paths& paths) {
    std::ifstream f(paths.profiles + "\\dp_dbstorage.cfg");
    if (!f) {
        log_line("конфиг не найден (profiles\\dp_dbstorage.cfg) — значения по "
                 "умолчанию, без соединения с СУБД не заработает");
        return {};
    }
    std::string text((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    const ConfigResult r = parse_config(text);
    if (!r.ok) {
        log_line("конфиг: " + r.error + " — значения по умолчанию");
        return {};
    }
    return r.cfg;
}

}  // namespace

void at_load() {
    Paths paths = resolve_paths();
    {
        auto& st = worker_state();
        std::lock_guard<std::mutex> lk(st.mx);
        st.paths = paths;
    }
    const DbConfig cfg = load_config(paths);
    {
        auto& st = worker_state();
        std::lock_guard<std::mutex> lk(st.mx);
        st.cfg = cfg;
    }
    log_line(std::format("загрузка: exe={}, profiles={}, mission={}",
                         paths.exe_dir, paths.profiles, paths.mission));
    log_line(std::format(
        "конфиг: {}@{}:{}/{} mode={} sync={}с", cfg.user, cfg.host, cfg.port,
        cfg.database, cfg.mode, cfg.sync_period_sec));

    if (cfg.mode == "takeover")
        takeover_wipe(paths);

    start_worker(cfg, paths);
}

}  // namespace dpdb::boot
