#include "boot.hpp"

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <format>
#include <fstream>

#include "graft/engine.hpp"
#include "graft/scan.hpp"

#include "db.hpp"
#include "db_mariadb.hpp"
#include "schema.hpp"
#include "sqlite.hpp"
#include "streams.hpp"
#include "worker.hpp"

namespace dpdb::boot {

namespace {

// Группы файлов ЖИВЫХ слоёв в storage_<id>/data — то, что восстанавливаем из
// MariaDB сами. types.*/doors.* СОЗНАТЕЛЬНО не трогаем: счётчики CE и состояния
// дверей остаются движку — по ним же он видит «существующий мир» и после хука
// globals не раскладывает бут-лут поверх нашего восстановления.
const char* kLayerGroups[] = {
    "dynamic_*", "vehicles.*", "zombies.*", "animals.*", "building*", "events.*",
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

void mkdirs_local(const std::string& dir) {
    std::string cur;
    for (std::size_t i = 0; i < dir.size(); ++i) {
        cur += dir[i];
        const bool sep = dir[i] == '\\' || dir[i] == '/';
        const bool last = i + 1 == dir.size();
        if ((sep || last) && cur.size() > 3)
            CreateDirectoryA(
                cur.substr(0, cur.size() - (last && !sep ? 0 : 1)).c_str(),
                nullptr);
    }
}

// ── CE: бут-раскладка лута не дублирует наш мир ─────────────────────────────
// Два хука (якоря и сигнатуры — порт DP_Storage, проверены на проде 1.30):
//   1) загрузчик globals.xml ("RespawnTypes"): RestartSpawn [obj+0x6c] = 0;
//   2) ВХОД нативного initial-прохода ("Initial Spawn is DISABLED"): если в
//      MariaDB есть мир — вызов движка пропускается целиком (мир восстановим
//      сами из БД); на честно свежем сервере (БД пуста) — раскладка работает.
using CeGlobalsApply = unsigned char (*)(void*, void*);
CeGlobalsApply g_ceGlobalsOrig = nullptr;

unsigned char CeGlobalsDetour(void* self, void* table) {
    const unsigned char r = g_ceGlobalsOrig(self, table);
    const auto obj = reinterpret_cast<std::uintptr_t>(self);
    if (graft::scan::readable(reinterpret_cast<void*>(obj + 0x6c), 4))
        *reinterpret_cast<int*>(obj + 0x6c) = 0;
    return r;
}

bool CeGlobalsHookInstall() {
    const auto fn = graft::scan::function_referencing("RespawnTypes");
    if (!fn)
        return false;
    if (!graft::scan::matches(*fn,
            graft::scan::sig<"48 89 5C 24 08 57 48 83 EC 20 48 8B FA 48 8B D9 "
                             "48 8B CF">))
        return false;
    return graft::hook(reinterpret_cast<void*>(*fn),
                       reinterpret_cast<void*>(&CeGlobalsDetour),
                       reinterpret_cast<void**>(&g_ceGlobalsOrig));
}

using CeInitialEntry = void (*)(void*, void*);
CeInitialEntry g_ceInitialOrig = nullptr;
long g_ceInitialSkipped = 0;   // сколько раз раскладка пропущена (для лога)

void CeInitialDetour(void* self, void* ctx) {
    if (db_has_world()) {
        ++g_ceInitialSkipped;
        return;   // мир восстановим из MariaDB — раскладку движка не зовём
    }
    g_ceInitialOrig(self, ctx);
}

bool CeInitialHookInstall() {
    const auto fn = graft::scan::function_referencing(
        "[CE][LootRespawner] (%s) :: Initial Spawn is DISABLED");
    if (!fn)
        return false;
    if (!graft::scan::matches(*fn,
            graft::scan::sig<"48 89 5C 24 08 57 48 83 EC 20 48 8B FA 48 8B D9 "
                             "E8 [rel32] 4C 8B 40 70">))
        return false;
    return graft::hook(reinterpret_cast<void*>(*fn),
                       reinterpret_cast<void*>(&CeInitialDetour),
                       reinterpret_cast<void**>(&g_ceInitialOrig));
}

// ── режим streams: persistence движка живёт в MariaDB ────────────────────────
// IAT-слот fopen движка переписывает пути storage_<id> в одноразовый кэш
// (profiles\dpdb_cache). Кэш наполняется из СУБД синхронно, ДО первого fopen
// движка; дальше воркер пушит изменения кэша и players.db в СУБД. Движок
// нативен во всём: CE-счётчики, «существующий мир», раскладки — никаких хуков
// поведения не нужно.
void boot_streams(const Paths& paths, const DbConfig& cfg) {
    if (!paths.resolved) {
        log_line("streams: миссия не найдена — перенаправление не включаем");
        return;
    }
    const std::string cache_dir = paths.profiles + "\\dpdb_cache";

    auto client = make_mariadb_client(cfg);
    if (!client->reconnect()) {
        log_line("streams: СУБД недоступна (" + client->error() +
                 ") — отказ: движок работает с настоящим storage");
        return;
    }
    {
        auto& st = worker_state();
        std::lock_guard<std::mutex> lk(st.mx);
        st.server_info = client->server_info();
    }
    std::string mig_log;
    if (!apply_migrations(*client, mig_log))
        log_line("streams: схема: " + mig_log);
    else if (mig_log.find("актуальна") == std::string::npos)
        log_line("streams: схема: " + mig_log);

    // 1) кэш-барьер: всё из СУБД (только отсутствующие в кэше файлы) —
    // включая players.db: в streams он живёт в кэше, настоящий путь движку
    // не нужен вовсе
    std::string err;
    const int pulled = cache_pull(*client, cache_dir, err);
    if (pulled < 0)
        log_line("streams: кэш не прочитан: " + err + " — продолжаем с тем, что есть");

    // 2) перенаправление: storage_<id>\... → кэш. Подкаталоги кэша — заранее:
    // движок зовёт CreateFileW без создания каталогов (каталоги он делает по
    // настоящему пути, который мы не трогаем).
    mkdirs_local(cache_dir + "\\data");
    mkdirs_local(cache_dir + "\\export");
    fopen_redirect_set(paths.storage, cache_dir);
    const int slots = fopen_iat_install();
    if (slots > 0)
        log_line(std::format(
            "streams: IAT fopen запатчен ({} слотов), кэш: {} файлов из СУБД; "
            "storage движка: {}",
            slots, std::max(pulled, 0), cache_dir));
    else
        log_line("streams: слотов fopen не найдено — перенаправление НЕ "
                 "работает, движок с настоящим storage");
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

    if (cfg.mode == "streams") {
        {
            auto& st = worker_state();
            std::lock_guard<std::mutex> lk(st.mx);
            st.cache_dir = paths.profiles + "\\dpdb_cache";
        }
        set_diag_logger([](const std::string& s) { log_line(s); });
        boot_streams(paths, cfg);   // барьер кэша до первого fopen движка
    } else if (cfg.mode == "takeover") {
        takeover_wipe(paths);
        // врезка законна только из ON_LOAD (движок ещё без потоков)
        if (CeGlobalsHookInstall())
            log_line("CE: хук globals стоит, RestartSpawn будет = 0");
        else
            log_line("CE: хук globals НЕ встал (сигнатура?) — бут-лут "
                     "контролирует только initial-хук");
        if (CeInitialHookInstall())
            log_line("CE: хук initial стоит — раскладка пропускается, когда в "
                     "MariaDB есть мир");
        else
            log_line("CE: хук initial НЕ встал — при восстановлении возможен "
                     "двойной бут-лут");
    }

    start_worker(cfg, paths);
}

}  // namespace dpdb::boot
