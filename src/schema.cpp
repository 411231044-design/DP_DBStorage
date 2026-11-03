#include "schema.hpp"

#include <charconv>
#include <format>

namespace dpdb {

const std::vector<Migration>& migrations() {
    static const std::vector<Migration> kMigrations = {
        {1,
         "база: игроки, мир, тени, команды",
         {
             // Метаданные: версия схемы, режим, сердцебиение воркера.
             "CREATE TABLE IF NOT EXISTS server_meta("
             "  k VARCHAR(64) PRIMARY KEY,"
             "  v TEXT"
             ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",

             // Персонажи: построчно, полный снапшот = DELETE+INSERT (store.cpp).
             // Дерево предметов — явные (uid, idx)/(parent_idx), без AUTO_INCREMENT:
             // ссылки видны прямо в SQL и не требуют LAST_INSERT_ID.
             "CREATE TABLE IF NOT EXISTS players("
             "  uid VARCHAR(64) PRIMARY KEY,"
             "  name VARCHAR(64),"
             "  alive TINYINT NOT NULL DEFAULT 1,"
             "  x DOUBLE, y DOUBLE, z DOUBLE, yaw FLOAT,"
             "  health FLOAT, blood FLOAT, water FLOAT, energy FLOAT,"
             "  lastseen DATETIME, created DATETIME"
             ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",

             "CREATE TABLE IF NOT EXISTS player_items("
             "  uid VARCHAR(64) NOT NULL,"
             "  idx INT NOT NULL,"
             "  parent_idx INT NOT NULL DEFAULT 0,"
             "  slot VARCHAR(64),"
             "  type VARCHAR(96) NOT NULL,"
             "  health FLOAT, quantity FLOAT, ammo INT,"
             "  liquid_type INT, liquid_vol FLOAT, food_stage INT,"
             "  wetness FLOAT, temperature FLOAT, extra TEXT,"
             "  PRIMARY KEY(uid, idx),"
             "  FOREIGN KEY (uid) REFERENCES players(uid) ON DELETE CASCADE"
             ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",

             // Слой мира: сущности + их предметы, ключ (layer, idx).
             "CREATE TABLE IF NOT EXISTS world_entities("
             "  layer VARCHAR(16) NOT NULL,"
             "  idx INT NOT NULL,"
             "  type VARCHAR(96) NOT NULL,"
             "  x DOUBLE, y DOUBLE, z DOUBLE,"
             "  yaw FLOAT, pitch FLOAT, roll FLOAT,"
             "  health FLOAT, updated DATETIME,"
             "  PRIMARY KEY(layer, idx),"
             "  KEY idx_world_type (type)"
             ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",

             "CREATE TABLE IF NOT EXISTS entity_items("
             "  layer VARCHAR(16) NOT NULL,"
             "  entity_idx INT NOT NULL,"
             "  idx INT NOT NULL,"
             "  parent_idx INT NOT NULL DEFAULT 0,"
             "  slot VARCHAR(64),"
             "  type VARCHAR(96) NOT NULL,"
             "  health FLOAT, quantity FLOAT, ammo INT,"
             "  liquid_type INT, liquid_vol FLOAT, food_stage INT,"
             "  wetness FLOAT, temperature FLOAT, extra TEXT,"
             "  PRIMARY KEY(layer, entity_idx, idx),"
             "  FOREIGN KEY (layer, entity_idx) REFERENCES world_entities(layer, idx)"
             "    ON DELETE CASCADE"
             ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",

             // Машины: жидкости и прочее, чего нет в общих полях сущности.
             "CREATE TABLE IF NOT EXISTS vehicles_extra("
             "  layer VARCHAR(16) NOT NULL,"
             "  entity_idx INT NOT NULL,"
             "  fuel FLOAT, oil FLOAT, coolant FLOAT, brake FLOAT,"
             "  extra TEXT,"
             "  PRIMARY KEY(layer, entity_idx),"
             "  FOREIGN KEY (layer, entity_idx) REFERENCES world_entities(layer, idx)"
             "    ON DELETE CASCADE"
             ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",

             // Теневые копии движковых байтов: страховка для аварийного отката
             // (players.db из SQLite-обёртки и файлы storage_1/data/*.bin).
             // hash (FNV-1a) — чтобы не перегонять неизменённые мегабайты.
             "CREATE TABLE IF NOT EXISTS shadow_players("
             "  uid VARCHAR(64) PRIMARY KEY,"
             "  data LONGBLOB,"
             "  bytes INT NOT NULL DEFAULT 0,"
             "  hash BIGINT,"
             "  updated DATETIME"
             ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",

             "CREATE TABLE IF NOT EXISTS shadow_files("
             "  name VARCHAR(128) PRIMARY KEY,"
             "  data LONGBLOB,"
             "  bytes INT NOT NULL DEFAULT 0,"
             "  hash BIGINT,"
             "  updated DATETIME"
             ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",

             // Управление из любого mysql-клиента (как storage_commands у DP).
             "CREATE TABLE IF NOT EXISTS db_commands("
             "  id BIGINT AUTO_INCREMENT PRIMARY KEY,"
             "  cmd VARCHAR(64) NOT NULL,"
             "  params VARCHAR(255),"
             "  status VARCHAR(16) NOT NULL DEFAULT 'new',"
             "  result TEXT,"
             "  created DATETIME DEFAULT CURRENT_TIMESTAMP,"
             "  updated DATETIME ON UPDATE CURRENT_TIMESTAMP"
             ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",

             // Точки отката (фаза 3).
             "CREATE TABLE IF NOT EXISTS snapshots("
             "  id BIGINT AUTO_INCREMENT PRIMARY KEY,"
             "  kind VARCHAR(32),"
             "  note VARCHAR(255),"
             "  ts DATETIME"
             ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",

             // Представления для людей.
             "CREATE OR REPLACE VIEW v_players AS "
             "SELECT uid, name, alive, x, y, z, health, blood, water, energy,"
             "       (SELECT COUNT(*) FROM player_items i WHERE i.uid = p.uid)"
             "         AS items,"
             "       lastseen, created "
             "FROM players p",

             "CREATE OR REPLACE VIEW v_world_summary AS "
             "SELECT layer, COUNT(*) AS entities, MAX(updated) AS updated"
             "  FROM world_entities GROUP BY layer",
         }},
        {2,
         "снапшоты: копии shadow_files для отката",
         {
             "CREATE TABLE IF NOT EXISTS snapshots_files("
             "  snapshot_id BIGINT NOT NULL,"
             "  name VARCHAR(128) NOT NULL,"
             "  data LONGBLOB,"
             "  bytes INT,"
             "  hash BIGINT,"
             "  PRIMARY KEY(snapshot_id, name)"
             ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",

             "CREATE OR REPLACE VIEW v_help AS "
             "SELECT 'wipe_player <uid>' AS cmd, 'вайп персонажа (+кик онлайн)'"
             "   AS what "
             "UNION ALL SELECT 'wipe_all_players', 'вайп всех персонажей' "
             "UNION ALL SELECT 'snapshot <note>', 'копия shadow_files в "
             "snapshots_files' "
             "UNION ALL SELECT 'status', 'сводка по хранилищу'",
         }},
        {3,
         "события мира: журнал спавнов/перемещений/смертей",
         {
             // Журнал событий: пишется пачками из буфера плагина, читается —
             // по (type, ts) или (uid, ts). Без FK: скорость вставки важнее.
             "CREATE TABLE IF NOT EXISTS world_events("
             "  id BIGINT AUTO_INCREMENT PRIMARY KEY,"
             "  ts DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),"
             "  type VARCHAR(32) NOT NULL,"
             "  uid VARCHAR(64),"
             "  etype VARCHAR(96),"
             "  x DOUBLE, y DOUBLE, z DOUBLE,"
             "  payload TEXT,"
             "  KEY idx_ev_type_ts (type, ts),"
             "  KEY idx_ev_uid_ts (uid, ts)"
             ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
         }},
    };
    return kMigrations;
}

namespace {

// -1 = СУБД недоступна; 0 = база чистая; иначе — записанная версия.
int applied_version(DbClient& db) {
    std::vector<DbRow> rows;
    if (!db.query("SELECT COUNT(*) FROM information_schema.tables"
                  " WHERE table_schema = DATABASE()"
                  "   AND table_name = 'server_meta'",
                  rows))
        return -1;
    bool exists = false;
    if (!rows.empty() && !rows.front().cols.empty())
        exists = rows.front().cols[0].index() == 1
                     ? std::get<std::int64_t>(rows.front().cols[0]) != 0
                     : true;   // строка есть — таблица есть (число не распарсили)
    if (!exists)
        return 0;
    rows.clear();
    if (!db.query("SELECT v FROM server_meta WHERE k = 'schema_version'", rows))
        return -1;
    if (rows.empty() || rows.front().cols.empty())
        return 0;
    const auto& v = rows.front().cols[0];
    if (const auto* s = std::get_if<std::string>(&v)) {
        int out = 0;
        const auto [ptr, ec] =
            std::from_chars(s->data(), s->data() + s->size(), out);
        if (ec == std::errc() && ptr == s->data() + s->size())
            return out;
        return 0;
    }
    if (const auto* i = std::get_if<std::int64_t>(&v))
        return static_cast<int>(*i);
    return 0;
}

}  // namespace

bool apply_migrations(DbClient& db, std::string& log) {
    const int current = applied_version(db);
    if (current < 0) {
        log = "СУБД недоступна: " + db.error();
        return false;
    }
    for (const auto& m : migrations()) {
        if (m.version <= current)
            continue;
        for (const auto& stmt : m.statements)
            if (!db.exec(stmt)) {
                log = "миграция v" + std::to_string(m.version) + " («" + m.name +
                      "»): " + db.error() + " [оператор: " +
                      stmt.substr(0, 400) + "]";
                return false;
            }
        if (!db.exec(std::format(
                "INSERT INTO server_meta(k, v) VALUES('schema_version', '{}') "
                "ON DUPLICATE KEY UPDATE v = VALUES(v)",
                m.version))) {
            log = "не записать версию схемы: " + db.error();
            return false;
        }
        log += std::format("v{} ({}): {} операторов; ", m.version, m.name,
                           m.statements.size());
    }
    if (log.empty())
        log = "схема актуальна (v" + std::to_string(current) + ")";
    return true;
}

}  // namespace dpdb
