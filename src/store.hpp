// Очередь снапшотов: staging нативов (сим-поток) → coalesce по uid/слою →
// SQL-воркер. Снапшот = полная замена (DELETE+INSERT в транзакции): просто,
// безопасно, не копит расхождения с базой.
#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "db.hpp"

namespace dpdb {

inline constexpr int kMaxPlayerItems = 4096;    // предметов на снапшот игрока
inline constexpr int kMaxWorldItems = 512;      // предметов на сущность мира
inline constexpr int kMaxEntities = 200000;     // сущностей на слой

// Предмет: узел дерева инвентаря. Отрицательные int/float = «нет» → NULL.
struct ItemRow {
    int parent = 0;                 // 0 = корень (слот самой сущности/игрока)
    std::string slot, type, extra;  // extra — длинный хвост, TEXT
    float health = 0, quantity = -1, liquid_vol = -1, wetness = 0, temperature = 0;
    int ammo = -1, liquid_type = -1, food_stage = -1;
};

struct PlayerSnap {
    std::string uid, name;
    int alive = 1;
    float x = 0, y = 0, z = 0, yaw = 0;
    float health = 0, blood = 0, water = 0, energy = 0;
    std::vector<ItemRow> items;
};

struct EntitySnap {
    std::string type;
    float x = 0, y = 0, z = 0, yaw = 0, pitch = 0, roll = 0, health = 0;
    std::vector<ItemRow> items;
};

struct WorldSnap {
    std::string layer;
    std::vector<EntitySnap> entities;
};

class Store {
public:
    // ── сим-поток: staging нативов. "" = ок, "ERROR: …" — скрипту в лог ────
    std::string player_begin(std::string_view uid, std::string_view name,
                             int alive, float x, float y, float z, float yaw,
                             float health, float blood, float water,
                             float energy);
    int player_item(const ItemRow& row);   // 1-основанный индекс, 0 = отказ
    std::string player_end();

    std::string world_begin(std::string_view layer);
    int world_entity(std::string_view type, float x, float y, float z,
                     float yaw, float pitch, float roll, float health);
    int world_item(int entity, const ItemRow& row);   // индекс внутри сущности
    std::string world_end();

    // ── воркер ──────────────────────────────────────────────────────────────
    void drain(DbClient& db, std::string& report);
    size_t pending_players() const;
    size_t pending_worlds() const;

    struct Stats {
        std::uint64_t players = 0, worlds = 0, items = 0, failed = 0;
        std::uint64_t item_errors = 0;   // предметы, отклонённые валидацией
        std::string last_error;          // последняя ошибка записи
        std::string last_item_error;     // последний отклонённый предмет
    };
    Stats stats() const;

private:
    std::string check_item(const ItemRow& row) const;

    mutable std::mutex mx_;
    std::optional<PlayerSnap> player_stage_;
    std::optional<WorldSnap> world_stage_;
    std::map<std::string, PlayerSnap> player_q_;   // coalesce: последний uid
    std::map<std::string, WorldSnap> world_q_;     // coalesce: последний слой
    Stats stats_;
};

// Глобальный стор плагина (нативы и воркер).
Store& store();

}  // namespace dpdb
