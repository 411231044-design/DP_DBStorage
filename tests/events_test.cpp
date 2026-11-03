// Журнал событий мира: буфер с капом, пачечная выгрузка, возврат хвоста при
// сбое СУБД. Схема v3 (world_events) присутствует в миграциях.
#include "store.hpp"
#include "schema.hpp"

#include <gtest/gtest.h>

#include "fake_db.hpp"

namespace {

dpdb::Store::EventRow ev(const char* type, float x = 1.f) {
    dpdb::Store::EventRow e;
    e.type = type;
    e.etype = "SodaCan_Cola";
    e.x = x;
    e.y = 2;
    e.z = 3;
    return e;
}

}  // namespace

TEST(Events, PushFlushBatched) {
    dpdb::Store s;
    for (int i = 0; i < 1200; ++i)
        ASSERT_TRUE(s.event_push(ev("item_spawn", float(i))));
    ASSERT_EQ(s.events_pending(), size_t{1200});

    dpdb::test::FakeDb db;
    s.events_flush(db);
    // пачки по 500: 1200 = 500 + 500 + 200
    EXPECT_EQ(db.count_prefix("INSERT INTO world_events"), size_t{3});
    EXPECT_EQ(s.events_pending(), size_t{0});
    EXPECT_EQ(s.stats().events, std::uint64_t{1200});
    // строка пачки содержит экранированные поля
    ASSERT_FALSE(db.log.empty());
    EXPECT_NE(db.log[0].find("'item_spawn'"), std::string::npos);
    EXPECT_NE(db.log[0].find("'SodaCan_Cola'"), std::string::npos);
}

TEST(Events, CapDrops) {
    dpdb::Store s;
    for (int i = 0; i < dpdb::kMaxEvents + 500; ++i)
        s.event_push(ev("item_move"));
    EXPECT_EQ(s.events_pending(), size_t{dpdb::kMaxEvents});
    EXPECT_EQ(s.stats().events_dropped, std::uint64_t{500});
    EXPECT_FALSE(s.event_push(ev("item_move")));   // буфер всё ещё полон
}

TEST(Events, FailedFlushReturnsTail) {
    dpdb::Store s;
    for (int i = 0; i < 700; ++i)
        s.event_push(ev("item_spawn"));
    dpdb::test::FakeDb db;
    db.errors.push_back("connection lost");   // первая же пачка падает
    s.events_flush(db);
    // ничего не записано, всё вернулось в буфер
    EXPECT_EQ(s.stats().events, std::uint64_t{0});
    EXPECT_EQ(s.events_pending(), size_t{700});
}

TEST(Events, BadTypeRejected) {
    dpdb::Store s;
    EXPECT_FALSE(s.event_push(ev("")));
}

TEST(Schema, WorldEventsTableInV3) {
    const auto& ms = dpdb::migrations();
    ASSERT_GE(ms.size(), size_t{3});
    std::string joined;
    for (const auto& m : ms)
        for (const auto& q : m.statements)
            joined += q + "\n";
    EXPECT_NE(joined.find("CREATE TABLE IF NOT EXISTS world_events"),
              std::string::npos);
    EXPECT_NE(joined.find("idx_ev_type_ts"), std::string::npos);
}
