// Нативы 3_Game: сохранение игрока/мира (стадийное, как StorageDump* у DP) и
// диагностика. Всё тяжёлое — в воркере, нативы только наполняют staging.
#include <format>
#include <string>
#include <string_view>

#include "graft/native.hpp"

#include "db.hpp"
#include "db_mariadb.hpp"
#include "restore.hpp"
#include "sqlite.hpp"
#include "streams.hpp"
#include "store.hpp"
#include "worker.hpp"

namespace {

using namespace dpdb;

// Режим плагина: streams — persistence движка перенаправлен в MariaDB
// (восстановление нативное, скриптовые рестореры не нужны), takeover/mirror —
// скриптовые сохранение/восстановление. Причина неактивного IAT видна прямо
// здесь (см. MARIADB-DEBUG-REQUEST: молчаливый отказ искали не в том логе).
std::string DbModeText() {
    auto& st = worker_state();
    std::lock_guard<std::mutex> lk(st.mx);
    std::string out = st.cfg.mode;
    if (st.cfg.mode == "streams")
        out += fopen_redirect_active()
                   ? " (IAT активен)"
                   : " (IAT НЕ активен: " +
                         (st.streams_note.empty() ? "нет данных" : st.streams_note) +
                         ")";
    return out;
}

// ── диагностика ──────────────────────────────────────────────────────────────

std::string DbVersion() {
    auto& st = worker_state();
    std::lock_guard<std::mutex> lk(st.mx);
    return std::format(
        "DP_DBStorage 1 (graft): mariadb client «{}», winsqlite «{}», phase «{}»"
        "{}, сервер: {}",
        mariadb_client_version(), sqlite::version(), st.phase,
        st.server_info.empty() ? ", соединения нет" : ", соединение есть",
        st.server_info.empty() ? "-" : st.server_info);
}

std::string DbStatus() {
    auto& st = worker_state();
    const auto stats = store().stats();
    std::lock_guard<std::mutex> lk(st.mx);
    return std::format(
        "phase={}, cycles={}, redirects={} createfile_storage={}; pending: "
        "players={} worlds={}; flushed: players={} worlds={} items={} "
        "failed={}; item_errors={}",
        st.phase, st.cycles, fopen_redirect_count(),
        createfile_storage_hits(), store().pending_players(),
        store().pending_worlds(), stats.players, stats.worlds, stats.items,
        stats.failed, stats.item_errors) +
        (stats.last_item_error.empty()
             ? std::string()
             : " [" + stats.last_item_error + "]") +
        (st.last_error.empty() ? std::string() : " last_error=" + st.last_error) +
        " report=" + st.last_report;
}

// ── сохранение игрока: полный снапшот заменяет предыдущий ────────────────────

std::string DbPlayerBegin(std::string_view uid, std::string_view name, int alive,
                          float x, float y, float z, float yaw, float health,
                          float blood, float water, float energy) {
    return store().player_begin(uid, name, alive, x, y, z, yaw, health, blood,
                                water, energy);
}

int DbPlayerItem(int parent, std::string_view slot, std::string_view type,
                 float health, float quantity, int ammo, int liquid_type,
                 float liquid_vol, int food_stage, float wetness,
                 float temperature, std::string_view extra) {
    ItemRow row;
    row.parent = parent;
    row.slot = slot;
    row.type = type;
    row.extra = extra;
    row.health = health;
    row.quantity = quantity;
    row.ammo = ammo;
    row.liquid_type = liquid_type;
    row.liquid_vol = liquid_vol;
    row.food_stage = food_stage;
    row.wetness = wetness;
    row.temperature = temperature;
    return store().player_item(row);
}

std::string DbPlayerEnd() {
    return store().player_end();
}

// ── сохранение мира: одна сессия собирает все слои сразу (один проход
// решётки по карте), End кладёт каждый отмеченный слой отдельным снапшотом ──

std::string DbWorldBegin() {
    return store().world_begin();
}

std::string DbWorldMark(std::string_view layer) {
    return store().world_mark(layer);
}

int DbWorldEntity(std::string_view layer, std::string_view type, float x,
                  float y, float z, float yaw, float pitch, float roll,
                  float health) {
    return store().world_entity(layer, type, x, y, z, yaw, pitch, roll, health);
}

int DbWorldItem(int handle, int parent, std::string_view slot,
                std::string_view type, float health, float quantity, int ammo,
                int liquid_type, float liquid_vol, int food_stage, float wetness,
                float temperature, std::string_view extra) {
    ItemRow row;
    row.parent = parent;
    row.slot = slot;
    row.type = type;
    row.extra = extra;
    row.health = health;
    row.quantity = quantity;
    row.ammo = ammo;
    row.liquid_type = liquid_type;
    row.liquid_vol = liquid_vol;
    row.food_stage = food_stage;
    row.wetness = wetness;
    row.temperature = temperature;
    return store().world_item(handle, row);
}

std::string DbWorldEnd() {
    return store().world_end();
}

// Число из конфига для скриптовых таймеров (-1 = нет такого ключа).
int DbConfigValue(std::string_view key) {
    auto& st = worker_state();
    std::lock_guard<std::mutex> lk(st.mx);
    const DbConfig& c = st.cfg;
    if (key == "sync_period_sec")
        return c.sync_period_sec;
    if (key == "player_save_sec")
        return c.player_save_sec;
    if (key == "world_save_sec")
        return c.world_save_sec;
    if (key == "port")
        return c.port;
    return -1;
}

// ── восстановление: строки из кэша (сеть — только в воркере) ─────────────────
// Слой: DbRestoreCount → -1 = не готов (скрипт повторяет позже); сущности и
// предметы — TAB-упакованные строки (Split("\t")).

int DbRestoreCount(std::string_view layer) {
    return restore().count(layer);
}

std::string DbRestoreEntity(std::string_view layer, int index) {
    return restore().entity(layer, index);
}

int DbRestoreItemsCount(std::string_view layer, int index) {
    return restore().items_count(layer, index);
}

std::string DbRestoreItem(std::string_view layer, int index, int item_idx) {
    return restore().item(layer, index, item_idx);
}

// Игрок: запрос уходит воркеру, готовность — опросом ("" = ждём, "MISS" = нет).
void DbPlayerRequest(std::string_view uid) {
    restore().request_player(uid);
}

std::string DbPlayerLoad(std::string_view uid) {
    return restore().player(uid);
}

int DbPlayerItemsCount(std::string_view uid) {
    return restore().player_items_count(uid);
}

std::string DbPlayerLoadItem(std::string_view uid, int item_idx) {
    return restore().player_item(uid, item_idx);
}

}  // namespace

GRAFT_BINDINGS("3_Game") {
    bind.global<&DbVersion>("DbVersion");
    bind.global<&DbStatus>("DbStatus");
    bind.global<&DbModeText>("DbModeText");
    bind.global<&DbConfigValue>("DbConfigValue", "key");

    bind.global<&DbPlayerBegin>(
        "DbPlayerBegin",
        "uid, name, alive, x, y, z, yaw, health, blood, water, energy");
    bind.global<&DbPlayerItem>(
        "DbPlayerItem",
        "parent, slot, type, health, quantity, ammo, liquid_type, liquid_vol, "
        "food_stage, wetness, temperature, extra");
    bind.global<&DbPlayerEnd>("DbPlayerEnd");

    bind.global<&DbWorldBegin>("DbWorldBegin");
    bind.global<&DbWorldMark>("DbWorldMark", "layer");
    bind.global<&DbWorldEntity>(
        "DbWorldEntity", "layer, type, x, y, z, yaw, pitch, roll, health");
    bind.global<&DbWorldItem>(
        "DbWorldItem",
        "handle, parent, slot, type, health, quantity, ammo, liquid_type, "
        "liquid_vol, food_stage, wetness, temperature, extra");
    bind.global<&DbWorldEnd>("DbWorldEnd");

    bind.global<&DbRestoreCount>("DbRestoreCount", "layer");
    bind.global<&DbRestoreEntity>("DbRestoreEntity", "layer, index");
    bind.global<&DbRestoreItemsCount>("DbRestoreItemsCount", "layer, index");
    bind.global<&DbRestoreItem>("DbRestoreItem", "layer, index, item");

    bind.global<&DbPlayerRequest>("DbPlayerRequest", "uid");
    bind.global<&DbPlayerLoad>("DbPlayerLoad", "uid");
    bind.global<&DbPlayerItemsCount>("DbPlayerItemsCount", "uid");
    bind.global<&DbPlayerLoadItem>("DbPlayerLoadItem", "uid, item");
}
