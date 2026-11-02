// Кэш восстановления: чтение слоёв/игроков из СУБД → упаковка строк для
// Enforce (TAB-разделённые, Split("\t") на скриптовой стороне).
#include "restore.hpp"

#include <algorithm>

#include <gtest/gtest.h>

#include "fake_db.hpp"

namespace {

dpdb::DbRow row(std::initializer_list<dpdb::Val> cols) {
    dpdb::DbRow r;
    for (const auto& c : cols)
        r.cols.push_back(c);
    return r;
}

}  // namespace

TEST(Restore, LayerRoundtrip) {
    dpdb::test::FakeDb db;
    db.replies.push_back({
        row({"CivilianSedan", 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 8500.0, std::int64_t{1}}),
        row({"SeaChest", 10.0, 20.0, 30.0, 0.0, 0.0, 0.0, 100.0, std::int64_t{2}}),
    });
    dpdb::DbRow item1;
    // entity_idx, parent, slot, type, health, quantity, ammo, liquid, liq_vol,
    // food, wet, temp, extra
    item1.cols = {std::int64_t{1}, std::int64_t{0}, std::string(""),
                  std::string("SparkPlug"), 80.0, -1.0, std::int64_t{-1},
                  std::int64_t{-1}, -1.0, std::int64_t{-1}, 0.0, 0.0,
                  dpdb::Val{}};
    dpdb::DbRow item2;
    item2.cols = {std::int64_t{2}, std::int64_t{0}, std::string("C0_0"),
                  std::string("SodaCan_Cola"), 90.0, 45.0, std::int64_t{-1},
                  std::int64_t{-1}, -1.0, std::int64_t{-1}, 0.0, 0.0,
                  dpdb::Val{}};
    db.replies.push_back({item1, item2});

    dpdb::Restore r;
    std::string err;
    ASSERT_TRUE(r.load_layer(db, "vehicles", err)) << err;
    EXPECT_EQ(r.count("vehicles"), 2);
    EXPECT_EQ(r.count("zombies"), -1);
    EXPECT_TRUE(r.ready());

    const std::string ent = r.entity("vehicles", 0);
    EXPECT_NE(ent.find("CivilianSedan"), std::string::npos);
    EXPECT_NE(ent.find("8500"), std::string::npos);
    EXPECT_EQ(r.entity("vehicles", 2), "");          // за границей
    EXPECT_EQ(r.entity("zombies", 0), "");

    EXPECT_EQ(r.items_count("vehicles", 0), 1);
    const std::string it0 = r.item("vehicles", 0, 0);
    EXPECT_NE(it0.find("SparkPlug"), std::string::npos);
    EXPECT_EQ(std::count(it0.begin(), it0.end(), '\t'), 11);   // 12 полей
    EXPECT_EQ(r.item("vehicles", 0, 1), "");
    EXPECT_EQ(r.items_count("vehicles", 1), 1);      // SeaChest с карго
}

TEST(Restore, PlayerLoadAndMiss) {
    dpdb::test::FakeDb db;
    dpdb::DbRow p;
    p.cols = {std::int64_t{1}, 100.0, 200.0, 300.0, 90.0, 95.0, 5000.0, 2500.0,
              1500.0, std::string("Tester")};
    db.replies.push_back({p});
    dpdb::DbRow item;
    item.cols = {std::int64_t{0}, std::string(""), std::string("TShirt_Beige"),
                 100.0, -1.0, std::int64_t{-1}, std::int64_t{-1}, -1.0,
                 std::int64_t{-1}, 0.0, 0.0, dpdb::Val{}};
    db.replies.push_back({item});

    dpdb::Restore r;
    std::string err;
    ASSERT_EQ(r.load_player(db, "pP0uid_test_1234", err), 1) << err;
    const std::string packed = r.player("pP0uid_test_1234");
    EXPECT_NE(packed.find("Tester"), std::string::npos);
    EXPECT_NE(packed.find("5000"), std::string::npos);
    EXPECT_EQ(r.player_items_count("pP0uid_test_1234"), 1);
    EXPECT_NE(r.player_item("pP0uid_test_1234", 0).find("TShirt_Beige"),
              std::string::npos);

    // игрока нет в базе — "MISS" (скрипт даст свежего персонажа)
    dpdb::test::FakeDb db2;
    db2.replies.push_back({});
    db2.replies.push_back({});
    ASSERT_EQ(r.load_player(db2, "pP0missing_uid_9", err), 0);
    EXPECT_EQ(r.player("pP0missing_uid_9"), "MISS");
    EXPECT_EQ(r.player("pP0never_requested_"), "");   // не готов
}

TEST(Restore, RequestQueueDedup) {
    dpdb::Restore r;
    r.request_player("pP0uid_test_1234");
    r.request_player("pP0uid_test_1234");   // дубль в очереди не нужен
    std::string uid;
    EXPECT_TRUE(r.has_request(uid));
    EXPECT_EQ(uid, "pP0uid_test_1234");
    EXPECT_FALSE(r.has_request(uid));
}

TEST(Restore, PackFormatsStable) {
    dpdb::RestoredItem it;
    it.parent = 3;
    it.slot = "S42";
    it.type = "Ammo_556";
    it.health = 55.5f;
    const std::string p = dpdb::pack_item(it);
    // 12 полей, TAB-разделённые — формат фиксирует скриптовая сторона Split
    EXPECT_EQ(std::count(p.begin(), p.end(), '\t'), 11);
}
