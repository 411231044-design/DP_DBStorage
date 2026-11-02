#include "restore.hpp"

#include <cmath>
#include <format>

#include "validate.hpp"

namespace dpdb {

namespace {

// Ячейка SELECT → float/int/string без сюрпризов (NULL → sentinel).
float cell_f(const Val& v, float missing) {
    if (const auto* d = std::get_if<double>(&v))
        return static_cast<float>(*d);
    if (const auto* i = std::get_if<std::int64_t>(&v))
        return static_cast<float>(*i);
    return missing;
}

int cell_i(const Val& v, int missing) {
    if (const auto* i = std::get_if<std::int64_t>(&v))
        return static_cast<int>(*i);
    if (const auto* d = std::get_if<double>(&v))
        return static_cast<int>(*d);
    return missing;
}

std::string cell_s(const Val& v) {
    if (const auto* s = std::get_if<std::string>(&v))
        return *s;
    return {};
}

std::string num(float v) {
    return std::isfinite(v) ? std::format("{}", static_cast<double>(v)) : "0";
}

RestoredItem item_from_row(const DbRow& r, std::size_t off) {
    // порядок колонок: parent, slot, type, health, quantity, ammo, liquid_type,
    // liquid_vol, food_stage, wetness, temperature, extra. Для entity_items
    // перед ними стоит entity_idx — offset.
    RestoredItem it;
    it.parent = cell_i(r.cols[off + 0], 0);
    it.slot = cell_s(r.cols[off + 1]);
    it.type = cell_s(r.cols[off + 2]);
    it.health = cell_f(r.cols[off + 3], 0);
    it.quantity = cell_f(r.cols[off + 4], -1);
    it.ammo = cell_i(r.cols[off + 5], -1);
    it.liquid_type = cell_i(r.cols[off + 6], -1);
    it.liquid_vol = cell_f(r.cols[off + 7], -1);
    it.food_stage = cell_i(r.cols[off + 8], -1);
    it.wetness = cell_f(r.cols[off + 9], 0);
    it.temperature = cell_f(r.cols[off + 10], 0);
    it.extra = cell_s(r.cols[off + 11]);
    return it;
}

}  // namespace

// Пустые поля пакуем как "-": Enforce Split выбрасывает пустые сегменты, и
// строка с пустым slot/extra расползалась бы на меньшее число полей.
std::string dash_if_empty(std::string_view s) {
    return s.empty() ? std::string("-") : std::string(s);
}

std::string pack_item(const RestoredItem& it) {
    // порядок полей фиксируем в README скриптовой части: parent, slot, type,
    // health, quantity, ammo, liquid_type, liquid_vol, food_stage, wetness,
    // temperature, extra; пустые slot/extra — дефис
    return std::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}",
                       it.parent, dash_if_empty(it.slot), it.type,
                       num(it.health), num(it.quantity), it.ammo, it.liquid_type,
                       num(it.liquid_vol), it.food_stage, num(it.wetness),
                       num(it.temperature), dash_if_empty(it.extra));
}

std::string pack_entity(const RestoredEntity& e) {
    return std::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}", e.type, num(e.x),
                       num(e.y), num(e.z), num(e.yaw), num(e.pitch),
                       num(e.roll), num(e.health));
}

std::string pack_player(const RestoredPlayer& p) {
    return std::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}", p.alive,
                       num(p.x), num(p.y), num(p.z), num(p.yaw), num(p.health),
                       num(p.blood), num(p.water), num(p.energy),
                       dash_if_empty(p.name));
}

// ── воркер ───────────────────────────────────────────────────────────────────

bool Restore::load_layer(DbClient& db, std::string_view layer, std::string& err) {
    if (!valid_layer(layer)) {
        err = "unknown layer";
        return false;
    }
    std::vector<DbRow> rows;
    if (!db.query(std::format(
            "SELECT type, x, y, z, yaw, pitch, roll, health, idx "
            "FROM world_entities WHERE layer = {} ORDER BY idx",
            sql_quote(layer)),
            rows)) {
        err = db.error();
        return false;
    }
    std::vector<RestoredEntity> ents;
    ents.reserve(rows.size());
    std::map<int, int> by_idx;   // idx сущности → позиция в ents
    for (const auto& r : rows) {
        RestoredEntity e;
        e.type = cell_s(r.cols[0]);
        e.x = cell_f(r.cols[1], 0);
        e.y = cell_f(r.cols[2], 0);
        e.z = cell_f(r.cols[3], 0);
        e.yaw = cell_f(r.cols[4], 0);
        e.pitch = cell_f(r.cols[5], 0);
        e.roll = cell_f(r.cols[6], 0);
        e.health = cell_f(r.cols[7], 0);
        by_idx[cell_i(r.cols[8], 0)] = static_cast<int>(ents.size());
        ents.push_back(std::move(e));
    }
    rows.clear();
    if (!db.query(std::format(
            "SELECT entity_idx, parent_idx, slot, type, health, quantity, ammo,"
            " liquid_type, liquid_vol, food_stage, wetness, temperature, extra,"
            " idx FROM entity_items WHERE layer = {} ORDER BY entity_idx, idx",
            sql_quote(layer)),
            rows)) {
        err = db.error();
        return false;
    }
    int attached = 0;
    for (const auto& r : rows) {
        const auto it = by_idx.find(cell_i(r.cols[0], -1));
        if (it == by_idx.end())
            continue;
        RestoredItem item = item_from_row(r, 1);   // cols[0] — entity_idx
        ents[it->second].items.push_back(std::move(item));
        ++attached;
    }
    std::lock_guard<std::mutex> lk(mx_);
    layers_[std::string(layer)] = std::move(ents);
    err = std::format("слой {}: {} сущностей, {} предметов", layer,
                      layers_[std::string(layer)].size(), attached);
    return true;
}

int Restore::load_player(DbClient& db, std::string_view uid, std::string& err) {
    if (!valid_uid(uid)) {
        err = "bad uid";
        return -1;
    }
    std::vector<DbRow> rows;
    if (!db.query(std::format(
            "SELECT alive, x, y, z, yaw, health, blood, water, energy, name"
            " FROM players WHERE uid = {}",
            sql_quote(uid)),
            rows)) {
        err = db.error();
        return -1;
    }
    std::lock_guard<std::mutex> lk(mx_);
    auto& slot = players_[std::string(uid)];
    if (rows.empty()) {
        slot.reset();
        return 0;
    }
    const auto& r = rows.front();
    RestoredPlayer p;
    p.uid = uid;
    p.alive = cell_i(r.cols[0], 1);
    p.x = cell_f(r.cols[1], 0);
    p.y = cell_f(r.cols[2], 0);
    p.z = cell_f(r.cols[3], 0);
    p.yaw = cell_f(r.cols[4], 0);
    p.health = cell_f(r.cols[5], 0);
    p.blood = cell_f(r.cols[6], 0);
    p.water = cell_f(r.cols[7], -1);
    p.energy = cell_f(r.cols[8], -1);
    p.name = cell_s(r.cols[9]);
    rows.clear();
    if (!db.query(std::format(
            "SELECT parent_idx, slot, type, health, quantity, ammo,"
            " liquid_type, liquid_vol, food_stage, wetness, temperature, extra"
            " FROM player_items WHERE uid = {} ORDER BY idx",
            sql_quote(uid)),
            rows)) {
        err = db.error();
        return -1;
    }
    for (const auto& row : rows)
        p.items.push_back(item_from_row(row, 0));
    slot = std::move(p);
    return 1;
}

size_t Restore::cached_layers() const {
    std::lock_guard<std::mutex> lk(mx_);
    return layers_.size();
}

// ── сим-поток ────────────────────────────────────────────────────────────────

bool Restore::ready() const {
    std::lock_guard<std::mutex> lk(mx_);
    return !layers_.empty();
}

int Restore::count(std::string_view layer) const {
    std::lock_guard<std::mutex> lk(mx_);
    const auto it = layers_.find(std::string(layer));
    return it == layers_.end() ? -1 : static_cast<int>(it->second.size());
}

std::string Restore::entity(std::string_view layer, int index) const {
    std::lock_guard<std::mutex> lk(mx_);
    const auto it = layers_.find(std::string(layer));
    if (it == layers_.end() || index < 0 ||
        index >= static_cast<int>(it->second.size()))
        return {};
    return pack_entity(it->second[index]);
}

int Restore::items_count(std::string_view layer, int index) const {
    std::lock_guard<std::mutex> lk(mx_);
    const auto it = layers_.find(std::string(layer));
    if (it == layers_.end() || index < 0 ||
        index >= static_cast<int>(it->second.size()))
        return -1;
    return static_cast<int>(it->second[index].items.size());
}

std::string Restore::item(std::string_view layer, int index, int item_idx) const {
    std::lock_guard<std::mutex> lk(mx_);
    const auto it = layers_.find(std::string(layer));
    if (it == layers_.end() || index < 0 ||
        index >= static_cast<int>(it->second.size()))
        return {};
    const auto& items = it->second[index].items;
    if (item_idx < 0 || item_idx >= static_cast<int>(items.size()))
        return {};
    return pack_item(items[item_idx]);
}

void Restore::request_player(std::string_view uid) {
    std::lock_guard<std::mutex> lk(mx_);
    // повторный запрос того же uid в очереди не нужен
    for (const auto& u : requests_)
        if (u == uid)
            return;
    if (const auto it = players_.find(std::string(uid)); it != players_.end())
        return;   // уже в кэше
    requests_.emplace_back(uid);
}

bool Restore::has_request(std::string& uid) {
    std::lock_guard<std::mutex> lk(mx_);
    if (requests_.empty())
        return false;
    uid = std::move(requests_.front());
    requests_.erase(requests_.begin());
    return true;
}

std::string Restore::player(std::string_view uid) const {
    std::lock_guard<std::mutex> lk(mx_);
    const auto it = players_.find(std::string(uid));
    if (it == players_.end())
        return {};          // ещё не готов
    if (!it->second.has_value())
        return "MISS";      // нет в базе — свежий персонаж
    return pack_player(*it->second);
}

int Restore::player_items_count(std::string_view uid) const {
    std::lock_guard<std::mutex> lk(mx_);
    const auto it = players_.find(std::string(uid));
    if (it == players_.end() || !it->second.has_value())
        return -1;
    return static_cast<int>(it->second->items.size());
}

std::string Restore::player_item(std::string_view uid, int item_idx) const {
    std::lock_guard<std::mutex> lk(mx_);
    const auto it = players_.find(std::string(uid));
    if (it == players_.end() || !it->second.has_value())
        return {};
    const auto& items = it->second->items;
    if (item_idx < 0 || item_idx >= static_cast<int>(items.size()))
        return {};
    return pack_item(items[item_idx]);
}

Restore& restore() {
    static Restore r;
    return r;
}

}  // namespace dpdb
