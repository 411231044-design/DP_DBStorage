#include "commands.hpp"

#include <format>
#include <fstream>

#include "sqlite.hpp"
#include "validate.hpp"

namespace dpdb {

namespace {

void finish(DbClient& db, std::int64_t id, const std::string& status,
            const std::string& result) {
    db.exec(std::format(
        "UPDATE db_commands SET status = {}, result = {}, updated = NOW()"
        " WHERE id = {}",
        sql_quote(status), sql_quote(result), id));
}

// Кик-флаг: uid на строку (или '*'); скрипт раз в 5 с разбирает и кикает.
void kick_append(const CommandEnv& env, const std::string& uid) {
    if (env.profiles_dir.empty())
        return;
    std::ofstream f(env.profiles_dir + "\\dp_db_kick.txt", std::ios::app);
    if (f)
        f << uid << "\n";
}

// Вайп персонажей в живом players.db (движок держит базу открытой —
// параллельный доступ проверен DP на проде). Возвращает текст результата.
std::string wipe_players(const CommandEnv& env, const std::string& uid_filter) {
    if (env.players_db.empty())
        return "ERROR: players.db путь не определён";
    sqlite::Db db;
    const std::string open_err = db.open(env.players_db, sqlite::kOpenReadWrite);
    if (!open_err.empty())
        return "ERROR: " + open_err;
    const std::string sql = uid_filter.empty()
                                ? std::string("DELETE FROM Players")
                                : "DELETE FROM Players WHERE UID = " +
                                      sql_quote(uid_filter);
    const std::string err = db.exec(sql);
    if (!err.empty())
        return "ERROR: " + err;
    const int n = db.changes();
    if (uid_filter.empty())
        kick_append(env, "*");
    else if (n > 0)
        kick_append(env, uid_filter);
    return std::format("OK deleted={}", n);
}

bool run_one(DbClient& db, const CommandEnv& env, std::int64_t id,
             const std::string& cmd, const std::string& params) {
    // разбор «имя params» одной строкой (правило Enforce к скриптам не
    // относится, но простота та же)
    const auto sp = params.find(' ');
    const std::string arg0 =
        sp == std::string::npos ? params : params.substr(0, sp);
    const std::string rest =
        sp == std::string::npos ? std::string() : params.substr(sp + 1);

    if (cmd == "wipe_player") {
        if (!valid_uid(arg0)) {
            finish(db, id, "error", "ERROR: bad uid '" + arg0 + "'");
            return true;
        }
        finish(db, id, "done", wipe_players(env, arg0));
        return true;
    }
    if (cmd == "wipe_all_players") {
        finish(db, id, "done", wipe_players(env, {}));
        return true;
    }
    if (cmd == "snapshot") {
        // заметка — вся строка params (arg0/rest резались по пробелу)
        std::string note = arg0;
        if (!rest.empty())
            note += " " + rest;
        if (!db.exec(std::format(
                "INSERT INTO snapshots(kind, note, ts) VALUES('shadow_files', "
                "{}, NOW())",
                sql_quote(note))))
            return false;   // СУБД — на следующий цикл
        std::vector<DbRow> rows;
        if (!db.query("SELECT LAST_INSERT_ID()", rows) || rows.empty())
            return false;
        const auto snap_id = std::get_if<std::int64_t>(&rows.front().cols[0]);
        if (!snap_id)
            return false;
        if (!db.exec(std::format(
                "INSERT INTO snapshots_files(snapshot_id, name, data, bytes,"
                " hash) SELECT {}, name, data, bytes, hash FROM shadow_files",
                *snap_id)))
            return false;
        finish(db, id, "done",
               std::format("OK snapshot={} (копия shadow_files)", *snap_id));
        return true;
    }
    if (cmd == "status") {
        std::vector<DbRow> rows;
        std::string summary = "OK";
        const auto as_i = [](const Val& v, std::int64_t d) -> std::int64_t {
            if (const auto* i = std::get_if<std::int64_t>(&v))
                return *i;
            if (const auto* f = std::get_if<double>(&v))   // SUM → DECIMAL
                return static_cast<std::int64_t>(*f);
            return d;
        };
        if (db.query("SELECT COUNT(*), COALESCE(SUM(bytes),0) FROM shadow_files",
                     rows) &&
            !rows.empty() && rows.front().cols.size() == 2)
            summary += std::format(" streams: {} файлов, {} байт",
                                   as_i(rows.front().cols[0], -1),
                                   as_i(rows.front().cols[1], -1));
        if (db.query("SELECT COUNT(*) FROM shadow_players", rows) &&
            !rows.empty() && !rows.front().cols.empty())
            summary += std::format(", игроков (тени): {}",
                                   as_i(rows.front().cols[0], -1));
        finish(db, id, "done", summary);
        return true;
    }
    finish(db, id, "error", "ERROR: unknown cmd '" + cmd + "' (см. v_help)");
    return true;
}

}  // namespace

void poll_commands(DbClient& db, const CommandEnv& env) {
    for (int guard = 0; guard < 32; ++guard) {
        std::vector<DbRow> rows;
        if (!db.query("SELECT id, cmd, params FROM db_commands"
                      " WHERE status = 'new' ORDER BY id LIMIT 1",
                      rows))
            return;   // СУБД/сеть — на следующий цикл
        if (rows.empty())
            return;
        std::int64_t id = 0;
        std::string cmd, params;
        if (rows.front().cols.size() == 3) {
            if (const auto* i = std::get_if<std::int64_t>(&rows.front().cols[0]))
                id = *i;
            if (const auto* s = std::get_if<std::string>(&rows.front().cols[1]))
                cmd = *s;
            if (const auto* s = std::get_if<std::string>(&rows.front().cols[2]))
                params = *s;
        }
        if (id == 0 || cmd.empty()) {
            finish(db, id, "error", "ERROR: bad row");
            continue;
        }
        // занять строку сразу: повторный цикл не возьмёт её второй раз
        db.exec(std::format("UPDATE db_commands SET status = 'run' WHERE id = {}", id));
        if (!run_one(db, env, id, cmd, params))
            db.exec(std::format(
                "UPDATE db_commands SET status = 'new' WHERE id = {}", id));
    }
}

}  // namespace dpdb
