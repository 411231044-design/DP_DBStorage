// Восстановление: воркер заранее читает снапшоты слоёв и игроков из MariaDB в
// кэш, нативы отдают строки скрипту без сети на сим-потоке. Игрок — по запросу
// (очередь к воркеру, скрипт опрашивает готовность).
#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "db.hpp"
#include "store.hpp"

namespace dpdb {

struct RestoredItem {
    int parent = 0;
    std::string slot, type, extra;
    float health = 0, quantity = -1, liquid_vol = -1, wetness = 0, temperature = 0;
    int ammo = -1, liquid_type = -1, food_stage = -1;
};

struct RestoredEntity {
    std::string type;
    float x = 0, y = 0, z = 0, yaw = 0, pitch = 0, roll = 0, health = 0;
    std::vector<RestoredItem> items;
};

struct RestoredPlayer {
    std::string uid, name;
    int alive = 1;
    float x = 0, y = 0, z = 0, yaw = 0;
    float health = 0, blood = 0, water = -1, energy = -1;
    std::vector<RestoredItem> items;
};

// Упаковка строки в TAB-разделённую строку для Enforce (Split("\t")).
std::string pack_entity(const RestoredEntity& e);
std::string pack_item(const RestoredItem& it);
std::string pack_player(const RestoredPlayer& p);

class Restore {
public:
    // ── воркер ──
    // Загрузить слой целиком (сущности + предметы). false — ошибка СУБД.
    bool load_layer(DbClient& db, std::string_view layer, std::string& err);
    // Загрузить игрока по uid; 0 — нет такого, -1 — ошибка СУБД, 1 — готово.
    int load_player(DbClient& db, std::string_view uid, std::string& err);
    // Сколько слоёв в кэше.
    size_t cached_layers() const;

    // ── сим-поток (нативы) ──
    bool ready() const;                          // хотя бы один слой загружен
    int count(std::string_view layer) const;     // -1 = слоя нет/не готов
    std::string entity(std::string_view layer, int index) const;   // "" = нет
    int items_count(std::string_view layer, int index) const;
    std::string item(std::string_view layer, int index, int item_idx) const;

    // Игрок: запрос ставится в очередь (заберёт воркер), статус — для скрипта.
    void request_player(std::string_view uid);
    bool has_request(std::string& uid);          // воркер забирает один запрос
    // "" = ещё не готов; "MISS" = нет в базе; иначе packed-строка
    std::string player(std::string_view uid) const;
    int player_items_count(std::string_view uid) const;
    std::string player_item(std::string_view uid, int item_idx) const;

private:
    mutable std::mutex mx_;
    std::map<std::string, std::vector<RestoredEntity>> layers_;
    std::map<std::string, std::optional<RestoredPlayer>> players_;  // nullopt = нет
    std::vector<std::string> requests_;
};

Restore& restore();

}  // namespace dpdb
