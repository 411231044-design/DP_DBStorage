#include "store.hpp"

#include <array>
#include <cmath>
#include <format>
#include <utility>

#include "validate.hpp"

namespace dpdb {

namespace {

// Дробное в SQL: без бесконечностей/NaN (скрипт мог передать мусор).
std::string f(float v) {
    return std::isfinite(v) ? std::format("{}", static_cast<double>(v)) : "NULL";
}

// Неотрицательное дробное: отрицательное = «поля нет» → NULL
// (quantity/liquid_vol/wetness отрицательными не бывают).
std::string opt_f(float v) {
    return v >= 0 && std::isfinite(v) ? std::format("{}", static_cast<double>(v))
                                      : "NULL";
}

// Неотрицательное целое: -1 = «поля нет» → NULL.
std::string opt_i(int v) {
    return v >= 0 ? std::to_string(v) : "NULL";
}

std::string text_or_null(const std::string& s) {
    return s.empty() ? "NULL" : sql_quote(s);
}

// Значения одной строки предмета (без uid/индексов): для player_items и
// entity_items колонки совпадают.
std::string item_values(const ItemRow& it) {
    return std::format("{},{},{},{},{},{},{},{},{},{},{},{}",
                       std::to_string(it.parent), text_or_null(it.slot),
                       sql_quote(it.type), f(it.health), opt_f(it.quantity),
                       opt_i(it.ammo), opt_i(it.liquid_type), opt_f(it.liquid_vol),
                       opt_i(it.food_stage), opt_f(it.wetness), f(it.temperature),
                       text_or_null(it.extra));
}

constexpr int kChunk = 128;   // строк на INSERT-пачку

std::vector<std::string> player_statements(const PlayerSnap& p) {
    std::vector<std::string> out;
    out.push_back("START TRANSACTION");
    out.push_back("DELETE FROM players WHERE uid=" + sql_quote(p.uid));
    out.push_back(std::format(
        "INSERT INTO players(uid,name,alive,x,y,z,yaw,health,blood,water,energy,"
        "lastseen,created) VALUES ({},{},{},{},{},{},{},{},{},{},{},NOW(),NOW())",
        sql_quote(p.uid), sql_quote(p.name), std::to_string(p.alive), f(p.x),
        f(p.y), f(p.z), f(p.yaw), f(p.health), f(p.blood), f(p.water),
        f(p.energy)));
    out.push_back("DELETE FROM player_items WHERE uid=" + sql_quote(p.uid));
    for (std::size_t i = 0; i < p.items.size(); i += kChunk) {
        std::string ins =
            "INSERT INTO player_items(uid,idx,parent_idx,slot,type,health,"
            "quantity,ammo,liquid_type,liquid_vol,food_stage,wetness,temperature,"
            "extra) VALUES ";
        for (std::size_t j = i; j < p.items.size() && j < i + kChunk; ++j)
            ins += std::format("({},{},{})", sql_quote(p.uid), j + 1,
                               item_values(p.items[j])) +
                   (j + 1 < p.items.size() && j + 1 < i + kChunk ? "," : "");
        out.push_back(std::move(ins));
    }
    out.push_back("COMMIT");
    return out;
}

std::vector<std::string> world_statements(const WorldSnap& w) {
    std::vector<std::string> out;
    out.push_back("START TRANSACTION");
    out.push_back("DELETE FROM world_entities WHERE layer=" + sql_quote(w.layer));
    for (std::size_t i = 0; i < w.entities.size(); i += kChunk) {
        std::string ins =
            "INSERT INTO world_entities(layer,idx,type,x,y,z,yaw,pitch,roll,"
            "health,updated) VALUES ";
        for (std::size_t j = i; j < w.entities.size() && j < i + kChunk; ++j) {
            const auto& e = w.entities[j];
            ins += std::format("({},{},{},{},{},{},{},{},{},{},NOW())",
                               sql_quote(w.layer), j + 1, sql_quote(e.type),
                               f(e.x), f(e.y), f(e.z), f(e.yaw), f(e.pitch),
                               f(e.roll), f(e.health)) +
                   (j + 1 < w.entities.size() && j + 1 < i + kChunk ? "," : "");
        }
        out.push_back(std::move(ins));
    }
    for (std::size_t i = 0; i < w.entities.size(); i += kChunk) {
        // предметы всех сущностей пачки — отдельными INSERT на сущность
        for (std::size_t j = i; j < w.entities.size() && j < i + kChunk; ++j) {
            const auto& items = w.entities[j].items;
            if (items.empty())
                continue;
            std::string ins =
                "INSERT INTO entity_items(layer,entity_idx,idx,parent_idx,slot,"
                "type,health,quantity,ammo,liquid_type,liquid_vol,food_stage,"
                "wetness,temperature,extra) VALUES ";
            for (std::size_t k = 0; k < items.size(); ++k)
                ins += std::format("({},{},{},{})", sql_quote(w.layer), j + 1,
                                   k + 1, item_values(items[k])) +
                       (k + 1 < items.size() ? "," : "");
            out.push_back(std::move(ins));
        }
    }
    out.push_back("COMMIT");
    return out;
}

}  // namespace

// ── staging: сим-поток ───────────────────────────────────────────────────────

std::string Store::check_item(const ItemRow& row) const {
    if (row.parent < 0)
        return "ERROR: item parent < 0";
    if (!valid_type_name(row.type))
        return "ERROR: bad type name '" + row.type + "'";
    if (!valid_slot(row.slot))
        return "ERROR: bad slot";
    if (!valid_text(row.extra, 4096))
        return "ERROR: bad extra";
    return "";
}

std::string Store::player_begin(std::string_view uid, std::string_view name,
                                int alive, float x, float y, float z, float yaw,
                                float health, float blood, float water,
                                float energy) {
    if (!valid_uid(uid))
        return "ERROR: bad uid '" + std::string(uid) + "'";
    if (!valid_text(name, 64))
        return "ERROR: bad player name";
    const std::array<float, 8> coords = {x, y, z, yaw, health, blood, water,
                                         energy};
    for (const float v : coords)
        if (!std::isfinite(v))
            return "ERROR: non-finite player scalar";
    std::lock_guard<std::mutex> lk(mx_);
    player_stage_.emplace();
    player_stage_->uid = uid;
    player_stage_->name = name;
    player_stage_->alive = alive ? 1 : 0;
    player_stage_->x = x;
    player_stage_->y = y;
    player_stage_->z = z;
    player_stage_->yaw = yaw;
    player_stage_->health = health;
    player_stage_->blood = blood;
    player_stage_->water = water;
    player_stage_->energy = energy;
    return "";
}

int Store::player_item(const ItemRow& row) {
    std::lock_guard<std::mutex> lk(mx_);
    if (!player_stage_)
        return 0;
    if (const std::string err = check_item(row); !err.empty()) {
        // предмет отклонён точечно (снапшот жив): ошибка — в статистику,
        // скрипт увидит через DbStatus и добьёт снятие повтором
        ++stats_.item_errors;
        stats_.last_item_error = err;
        return 0;
    }
    if (static_cast<int>(player_stage_->items.size()) >= kMaxPlayerItems)
        return 0;
    player_stage_->items.push_back(row);
    return static_cast<int>(player_stage_->items.size());
}

std::string Store::player_end() {
    std::lock_guard<std::mutex> lk(mx_);
    if (!player_stage_)
        return "ERROR: player_end без player_begin";
    player_q_[player_stage_->uid] = std::move(*player_stage_);
    player_stage_.reset();
    return "";
}

std::string Store::world_begin() {
    std::lock_guard<std::mutex> lk(mx_);
    world_stage_open_ = true;
    world_stage_.clear();
    world_handles_.clear();
    return "";
}

std::string Store::world_mark(std::string_view layer) {
    if (!valid_layer(layer))
        return "ERROR: unknown layer '" + std::string(layer) + "'";
    std::lock_guard<std::mutex> lk(mx_);
    if (!world_stage_open_)
        return "ERROR: world_mark без world_begin";
    auto [it, inserted] = world_stage_.try_emplace(std::string(layer));
    if (inserted)
        it->second.layer = layer;
    return "";
}

int Store::world_entity(std::string_view layer, std::string_view type, float x,
                        float y, float z, float yaw, float pitch, float roll,
                        float health) {
    if (!valid_layer(layer) || !valid_type_name(type))
        return 0;
    const std::array<float, 7> coords = {x, y, z, yaw, pitch, roll, health};
    for (const float v : coords)
        if (!std::isfinite(v))
            return 0;
    std::lock_guard<std::mutex> lk(mx_);
    if (!world_stage_open_)
        return 0;
    auto& snap = world_stage_[std::string(layer)];
    snap.layer = layer;
    if (static_cast<int>(snap.entities.size()) >= kMaxEntities)
        return 0;
    snap.entities.push_back(
        {std::string(type), x, y, z, yaw, pitch, roll, health, {}});
    world_handles_.emplace_back(layer,
                                static_cast<int>(snap.entities.size()));
    return static_cast<int>(world_handles_.size());
}

int Store::world_item(int handle, const ItemRow& row) {
    std::lock_guard<std::mutex> lk(mx_);
    if (!world_stage_open_ || handle < 1 ||
        handle > static_cast<int>(world_handles_.size()))
        return 0;
    if (const std::string err = check_item(row); !err.empty()) {
        ++stats_.item_errors;
        stats_.last_item_error = err;
        return 0;
    }
    const auto& [layer, idx] = world_handles_[handle - 1];
    auto& items = world_stage_.at(layer).entities[idx - 1].items;
    if (static_cast<int>(items.size()) >= kMaxWorldItems)
        return 0;
    items.push_back(row);
    return static_cast<int>(items.size());
}

std::string Store::world_end() {
    std::lock_guard<std::mutex> lk(mx_);
    if (!world_stage_open_)
        return "ERROR: world_end без world_begin";
    for (auto& [layer, snap] : world_stage_)
        world_q_[layer] = std::move(snap);
    world_stage_open_ = false;
    world_stage_.clear();
    world_handles_.clear();
    return "";
}

// ── воркер ───────────────────────────────────────────────────────────────────

void Store::drain(DbClient& db, std::string& report) {
    std::map<std::string, PlayerSnap> players;
    std::map<std::string, WorldSnap> worlds;
    {
        std::lock_guard<std::mutex> lk(mx_);
        players.swap(player_q_);
        worlds.swap(world_q_);
    }

    std::uint64_t ok_players = 0, ok_worlds = 0, ok_items = 0, failed = 0;
    std::string last_error;
    auto flush = [&](const std::vector<std::string>& stmts) {
        for (const auto& s : stmts)
            if (!db.exec(s)) {
                db.exec("ROLLBACK");   // результат не важен — база могла умереть
                last_error = db.error();
                ++failed;
                return false;
            }
        return true;
    };

    for (auto& [uid, snap] : players) {
        const auto stmts = player_statements(snap);
        if (flush(stmts)) {
            ++ok_players;
            ok_items += snap.items.size();
        }
    }
    for (auto& [layer, snap] : worlds) {
        const auto stmts = world_statements(snap);
        if (flush(stmts)) {
            ++ok_worlds;
            for (const auto& e : snap.entities)
                ok_items += e.items.size();
        }
    }

    {
        std::lock_guard<std::mutex> lk(mx_);
        stats_.players += ok_players;
        stats_.worlds += ok_worlds;
        stats_.items += ok_items;
        stats_.failed += failed;
        if (!last_error.empty())
            stats_.last_error = last_error;
    }
    report = std::format("players={} worlds={} items={} failed={}", ok_players,
                         ok_worlds, ok_items, failed);
}

size_t Store::pending_players() const {
    std::lock_guard<std::mutex> lk(mx_);
    return player_q_.size();
}

size_t Store::pending_worlds() const {
    std::lock_guard<std::mutex> lk(mx_);
    return world_q_.size();
}

Store::Stats Store::stats() const {
    std::lock_guard<std::mutex> lk(mx_);
    return stats_;
}

Store& store() {
    static Store s;
    return s;
}

}  // namespace dpdb
