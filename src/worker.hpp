// Фоновый поток: MySQL — только здесь (правило KR_GRAFTED: движок и нативы —
// на сим-потоке, сеть и диск — в воркере). Состояние для нативов — в
// worker_state() под мьютексом.
#pragma once
#include <cstdint>
#include <mutex>
#include <string>

#include "db.hpp"

namespace dpdb {

struct Paths {
    std::string exe_dir;      // каталог DayZServer*.exe
    std::string profiles;     // -profiles (или profiles/ рядом с exe)
    std::string mission;      // mpmissions/<template>
    std::string storage;      // mission/storage_1
    std::string data_dir;     // storage/data — слои *.bin
    std::string players_db;   // storage/players.db
    std::string log_file;     // profiles/dp_dbstorage.log
    bool resolved = false;    // миссия нашлась (без -config — частичный резолв)
};

// Резолв путей из командной строки сервера (-profiles, -config → template).
Paths resolve_paths();

// Журнал: profiles/dp_dbstorage.log, свой мьютекс (зовут оба потока).
void log_line(const std::string& msg);

// Общее состояние воркера для нативов (DbStatus).
struct WorkerState {
    std::mutex mx;
    std::string phase = "boot";         // boot → connect → schema → ready
    std::string server_info;            // версия MariaDB после коннекта
    std::string last_report;            // последний drain: players=N worlds=M…
    std::string last_error;
    std::uint64_t cycles = 0;
    DbConfig cfg;
    Paths paths;
    std::string cache_dir;              // режим streams: profiles\dpdb_cache
    std::string streams_note;           // почему redirect не активен ("" = активен)
};
WorkerState& worker_state();

// Тело потока (не возвращается): коннект → миграции → цикл
// (drain очереди, тени, сердцебиение). Запуск — start_worker (detach).
void worker_main(DbConfig cfg, Paths paths);
void start_worker(DbConfig cfg, Paths paths);

// Правда к моменту CE-бут-прохода: есть ли в MariaDB данные мира. Решает,
// пропускать ли раскладку лута движком (мир восстановим сами) или дать ей
// сработать (честно свежий сервер). Атомик: пишет воркер, читает детур.
bool db_has_world();
void set_db_has_world(bool yes);

}  // namespace dpdb
