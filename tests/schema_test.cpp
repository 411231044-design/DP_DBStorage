// Версионированные миграции схемы dp_storage. Накат на чистой базе — все
// операторы по порядку; база на последней версии — только чтение версии;
// недоступность СУБД — честный отказ, а не «версия 0».
#include "schema.hpp"

#include <gtest/gtest.h>

#include "fake_db.hpp"

using dpdb::migrations;

TEST(Schema, MigrationsList) {
    ASSERT_FALSE(migrations().empty());
    int prev = 0;
    for (const auto& m : migrations()) {
        EXPECT_GT(m.version, prev);      // версии строго возрастают с 1
        prev = m.version;
        EXPECT_NE(m.name, nullptr);
        ASSERT_FALSE(m.statements.empty());
    }
    EXPECT_EQ(prev, dpdb::kSchemaVersion);
}

TEST(Schema, ApplyOnFreshDb) {
    dpdb::test::FakeDb db;
    db.replies.push_back({});            // information_schema: строк нет → база чистая
    std::string log;
    ASSERT_TRUE(dpdb::apply_migrations(db, log)) << db.error();
    std::string joined;
    for (const auto& s : db.log)
        joined += s + "\n";
    for (const char* table : {"server_meta", "players", "player_items",
                              "world_entities", "entity_items", "vehicles_extra",
                              "shadow_players", "shadow_files", "db_commands",
                              "snapshots"})
        EXPECT_NE(joined.find(std::string("CREATE TABLE IF NOT EXISTS ") + table),
                  std::string::npos)
            << "нет таблицы " << table;
    EXPECT_NE(joined.find("CREATE OR REPLACE VIEW v_players"), std::string::npos);
    EXPECT_NE(joined.find("CREATE OR REPLACE VIEW v_world_summary"),
              std::string::npos);
    // версия записана
    bool versioned = false;
    for (const auto& s : db.log)
        if (s.rfind("INSERT", 0) == 0 &&
            s.find("schema_version") != std::string::npos)
            versioned = true;
    EXPECT_TRUE(versioned);
}

TEST(Schema, ReapplyIsNoop) {
    dpdb::test::FakeDb db;
    dpdb::DbRow count;
    count.cols.emplace_back(std::int64_t{1});   // server_meta существует
    db.replies.push_back({count});
    dpdb::DbRow ver;
    ver.cols.emplace_back(std::to_string(dpdb::kSchemaVersion));
    db.replies.push_back({ver});                // версия — последняя
    std::string log;
    ASSERT_TRUE(dpdb::apply_migrations(db, log));
    // только два чтения (наличие таблицы + версия), ни одного CREATE/INSERT
    EXPECT_EQ(db.log.size(), size_t{2});
    EXPECT_EQ(db.count_prefix("CREATE"), size_t{0});
}

TEST(Schema, ApplyFromVersionZero) {
    dpdb::test::FakeDb db;
    dpdb::DbRow count;
    count.cols.emplace_back(std::int64_t{1});
    db.replies.push_back({count});
    dpdb::DbRow ver;
    ver.cols.emplace_back(std::int64_t{0});     // база на версии 0
    db.replies.push_back({ver});
    std::string log;
    ASSERT_TRUE(dpdb::apply_migrations(db, log));
    EXPECT_GT(db.count_prefix("CREATE"), size_t{0});
}

TEST(Schema, UnreachableDbAborts) {
    dpdb::test::FakeDb db;
    db.replies.push_back({});
    db.errors.push_back("connection refused");  // первый же запрос падает
    std::string log;
    EXPECT_FALSE(dpdb::apply_migrations(db, log));
    EXPECT_FALSE(db.error().empty());
}
