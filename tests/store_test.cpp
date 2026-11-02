// Очередь снапшотов: staging нативов → coalesce по uid/слою → SQL воркера.
// Снапшот = полная замена (DELETE+INSERT в транзакции) — просто и безопасно.
#include "store.hpp"

#include <gtest/gtest.h>

#include <string>

#include "db.hpp"
#include "fake_db.hpp"

namespace {

dpdb::ItemRow item(int parent, const char* slot, const char* type,
                   float health = 100, float quantity = -1) {
    dpdb::ItemRow r;
    r.parent = parent;
    r.slot = slot;
    r.type = type;
    r.health = health;
    r.quantity = quantity;
    return r;
}

}  // namespace

TEST(Store, PlayerFlowEndToEnd) {
    dpdb::Store s;
    EXPECT_EQ(s.player_begin("pP0uid_test_1234", "Tester", 1, 100, 200, 300, 90,
                             95, 5000, 2500, 1500),
              "");
    EXPECT_EQ(s.player_item(item(0, "", "TShirt_Beige")), 1);
    EXPECT_EQ(s.player_item(item(1, "Body", "SodaCan_Cola", 80, 45)), 2);
    EXPECT_EQ(s.player_end(), "");
    EXPECT_EQ(s.pending_players(), size_t{1});
}

TEST(Store, PlayerInvalidUidRejected) {
    dpdb::Store s;
    EXPECT_NE(s.player_begin("ab", "X", 1, 0, 0, 0, 0, 0, 0, 0, 0).find("ERROR"),
              std::string::npos);
    EXPECT_EQ(s.player_item(item(0, "", "TShirt_Beige")), 0);  // сессии нет
    EXPECT_NE(s.player_end().find("ERROR"), std::string::npos);
}

TEST(Store, PlayerCoalescesByUid) {
    dpdb::Store s;
    s.player_begin("pP0uid_test_1234", "A", 1, 0, 0, 0, 0, 0, 0, 0, 0);
    s.player_item(item(0, "", "TShirt_Beige"));
    s.player_end();
    s.player_begin("pP0uid_test_1234", "A2", 1, 1, 1, 1, 0, 0, 0, 0, 0);
    s.player_end();
    EXPECT_EQ(s.pending_players(), size_t{1});  // в очереди последний
    dpdb::test::FakeDb db;
    std::string report;
    s.drain(db, report);
    // в SQL попало имя из последнего снапшота
    bool found = false;
    for (const auto& q : db.log)
        if (q.find("'A2'") != std::string::npos)
            found = true;
    EXPECT_TRUE(found);
}

TEST(Store, WorldFlowEndToEnd) {
    dpdb::Store s;
    EXPECT_EQ(s.world_begin(), "");
    EXPECT_EQ(s.world_entity("vehicles", "CivilianSedan", 100, 200, 300, 90, 0,
                             0, 8500),
              1);
    EXPECT_EQ(s.world_item(1, item(0, "SparkPlug", "SparkPlug")), 1);
    // второй слой в той же сессии: handle сквозной
    EXPECT_EQ(s.world_entity("dynamic", "SeaChest", 1, 2, 3, 0, 0, 0, 100), 2);
    EXPECT_EQ(s.world_item(2, item(0, "", "SodaCan_Cola")), 1);
    EXPECT_EQ(s.world_item(0, item(0, "", "SodaCan_Cola")), 0);  // нет handle 0
    EXPECT_EQ(s.world_end(), "");
    EXPECT_EQ(s.pending_worlds(), size_t{2});
}

TEST(Store, WorldMarkExplicitEmpty) {
    dpdb::Store s;
    EXPECT_EQ(s.world_begin(), "");
    EXPECT_EQ(s.world_mark("zombies"), "");
    EXPECT_EQ(s.world_end(), "");
    EXPECT_EQ(s.pending_worlds(), size_t{1});   // явная пустота = стереть слой
}

TEST(Store, WorldBadLayerRejected) {
    dpdb::Store s;
    EXPECT_EQ(s.world_begin(), "");
    EXPECT_NE(s.world_mark("types").find("ERROR"), std::string::npos);
    EXPECT_EQ(s.world_entity("types", "X", 0, 0, 0, 0, 0, 0, 0), 0);
    EXPECT_EQ(s.world_end(), "");
    EXPECT_EQ(s.pending_worlds(), size_t{0});   // ничего не отметили — пусто
}

TEST(Store, ItemCap) {
    dpdb::Store s;
    s.player_begin("pP0uid_test_1234", "A", 1, 0, 0, 0, 0, 0, 0, 0, 0);
    int last = 0;
    for (int i = 0; i < dpdb::kMaxPlayerItems + 100; ++i)
        last = s.player_item(item(0, "", "Ammo_556"));
    EXPECT_LT(last, dpdb::kMaxPlayerItems + 1);   // дальше 0 — переполнение
}

TEST(Store, DrainPlayerSql) {
    dpdb::Store s;
    s.player_begin("pP0uid_test_1234", "Tester", 1, 1.5f, 2, 3, 90, 95, 5000,
                   2500, 1500);
    s.player_item(item(0, "", "TShirt_Beige"));
    s.player_item(item(1, "Body", "SodaCan_Cola", 80, 45));
    s.player_end();

    dpdb::test::FakeDb db;
    std::string report;
    s.drain(db, report);
    ASSERT_GT(db.log.size(), size_t{5});
    EXPECT_EQ(db.log[0], "START TRANSACTION");
    EXPECT_EQ(db.log[1], "DELETE FROM players WHERE uid='pP0uid_test_1234'");
    EXPECT_NE(db.log[2].find("INSERT INTO players"), std::string::npos);
    EXPECT_NE(db.log[2].find("'Tester'"), std::string::npos);
    EXPECT_EQ(db.log[3], "DELETE FROM player_items WHERE uid='pP0uid_test_1234'");
    EXPECT_NE(db.log[4].find("INSERT INTO player_items"), std::string::npos);
    // обе строки предметов в одной пачке
    EXPECT_NE(db.log[4].find("'TShirt_Beige'"), std::string::npos);
    EXPECT_NE(db.log[4].find("'SodaCan_Cola'"), std::string::npos);
    EXPECT_EQ(db.log.back(), "COMMIT");
    EXPECT_EQ(s.stats().players, std::uint64_t{1});
}

TEST(Store, DrainWorldSqlCascade) {
    dpdb::Store s;
    s.world_begin();
    s.world_mark("vehicles");
    s.world_entity("vehicles", "CivilianSedan", 1, 2, 3, 4, 5, 6, 8500);
    s.world_item(1, item(0, "SparkPlug", "SparkPlug"));
    s.world_end();

    dpdb::test::FakeDb db;
    std::string report;
    s.drain(db, report);
    ASSERT_GT(db.log.size(), size_t{4});
    EXPECT_EQ(db.log[1], "DELETE FROM world_entities WHERE layer='vehicles'");
    // сущности раньше предметов (FK (layer,entity_idx) -> (layer,idx))
    const auto i_ent = db.count_prefix("INSERT INTO world_entities");
    const auto i_item = db.count_prefix("INSERT INTO entity_items");
    EXPECT_EQ(i_ent, size_t{1});
    EXPECT_EQ(i_item, size_t{1});
    size_t pos_ent = 0, pos_item = 0;
    for (size_t i = 0; i < db.log.size(); ++i) {
        if (db.log[i].rfind("INSERT INTO world_entities", 0) == 0)
            pos_ent = i;
        if (db.log[i].rfind("INSERT INTO entity_items", 0) == 0)
            pos_item = i;
    }
    EXPECT_LT(pos_ent, pos_item);
    EXPECT_EQ(db.log.back(), "COMMIT");
}

TEST(Store, DrainEmptyWorldLayer) {
    dpdb::Store s;
    s.world_begin();
    s.world_mark("zombies");
    s.world_end();
    dpdb::test::FakeDb db;
    std::string report;
    s.drain(db, report);
    EXPECT_EQ(db.log[1], "DELETE FROM world_entities WHERE layer='zombies'");
    EXPECT_EQ(db.count_prefix("INSERT INTO"), size_t{0});
    EXPECT_EQ(db.log.back(), "COMMIT");
}

TEST(Store, DrainChunksInserts) {
    dpdb::Store s;
    s.player_begin("pP0uid_test_1234", "A", 1, 0, 0, 0, 0, 0, 0, 0, 0);
    for (int i = 0; i < 300; ++i)
        s.player_item(item(0, "", "Ammo_556"));
    s.player_end();
    dpdb::test::FakeDb db;
    std::string report;
    s.drain(db, report);
    // пачки по 128 строк: 300 = 128 + 128 + 44
    EXPECT_EQ(db.count_prefix("INSERT INTO player_items"), size_t{3});
}

TEST(Store, DrainFailureRollsBackAndCounts) {
    dpdb::Store s;
    s.player_begin("pP0uid_test_1234", "A", 1, 0, 0, 0, 0, 0, 0, 0, 0);
    s.player_item(item(0, "", "TShirt_Beige"));
    s.player_end();

    dpdb::test::FakeDb db;
    db.replies.push_back({});      // migration-стайл не нужен, drain SELECT не делает
    db.errors.push_back("connection lost");
    std::string report;
    s.drain(db, report);
    // транзакция откачена, провал посчитан
    EXPECT_EQ(db.log.back(), "ROLLBACK");
    EXPECT_EQ(s.stats().failed, std::uint64_t{1});
    EXPECT_FALSE(s.stats().last_error.empty());
    EXPECT_EQ(s.pending_players(), size_t{0});   // снапшот не завис в очереди
}

TEST(Store, StageResetBySecondBegin) {
    dpdb::Store s;
    s.player_begin("pP0uid_test_1234", "A", 1, 0, 0, 0, 0, 0, 0, 0, 0);
    s.player_item(item(0, "", "TShirt_Beige"));
    s.player_begin("pP0other_uid_5678", "B", 1, 0, 0, 0, 0, 0, 0, 0, 0);
    s.player_end();
    dpdb::test::FakeDb db;
    std::string report;
    s.drain(db, report);
    // предмет из первой сессии не должен попасть во второй снапшот
    for (const auto& q : db.log)
        if (q.rfind("INSERT INTO player_items", 0) == 0)
            EXPECT_EQ(q.find("TShirt_Beige"), std::string::npos);
}
