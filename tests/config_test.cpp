// Парсер конфига $profile:dp_dbstorage.cfg — строки «ключ = значение».
// Строгий: неизвестный ключ — ошибка (опечатка не должна молча игнорироваться).
#include "db.hpp"

#include <gtest/gtest.h>

using dpdb::parse_config;

TEST(Config, DefaultsOnEmpty) {
    auto r = parse_config("");
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.cfg.host, "127.0.0.1");
    EXPECT_EQ(r.cfg.port, 3306);
    EXPECT_EQ(r.cfg.user, "dpstorage");
    EXPECT_EQ(r.cfg.password, "");
    EXPECT_EQ(r.cfg.database, "dp_storage");
    EXPECT_EQ(r.cfg.mode, "streams");
    EXPECT_EQ(r.cfg.sync_period_sec, 30);
    EXPECT_EQ(r.cfg.player_save_sec, 60);
    EXPECT_EQ(r.cfg.world_save_sec, 300);
}

TEST(Config, KeyValue) {
    auto r = parse_config("host = db.local\nport=3307\nuser=dp\npassword=secret 1\n"
                          "mode=mirror\ndatabase=dp2\n");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.cfg.host, "db.local");
    EXPECT_EQ(r.cfg.port, 3307);
    EXPECT_EQ(r.cfg.user, "dp");
    EXPECT_EQ(r.cfg.password, "secret 1");
    EXPECT_EQ(r.cfg.mode, "mirror");
    EXPECT_EQ(r.cfg.database, "dp2");
}

TEST(Config, CommentsAndBlankLines) {
    auto r = parse_config("# комментарий\n\n  port = 3310  # хвост\n");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.cfg.port, 3310);
}

TEST(Config, BomTolerated) {
    auto r = parse_config("\xEF\xBB\xBFport=3309\n");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.cfg.port, 3309);
}

TEST(Config, UnknownKeyIsError) {
    auto r = parse_config("hostx=1\n");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("hostx"), std::string::npos) << r.error;
}

TEST(Config, MissingKeyOrValue) {
    EXPECT_FALSE(parse_config("=5\n").ok);
    EXPECT_FALSE(parse_config("port=\n").ok);
    EXPECT_FALSE(parse_config("port\n").ok);
}

TEST(Config, BadPort) {
    EXPECT_FALSE(parse_config("port=0\n").ok);
    EXPECT_FALSE(parse_config("port=65536\n").ok);
    EXPECT_FALSE(parse_config("port=abc\n").ok);
}

TEST(Config, BadMode) {
    auto r = parse_config("mode=readonly\n");
    EXPECT_FALSE(r.ok);
    // ошибка обязана подсказать допустимые режимы
    EXPECT_NE(r.error.find("takeover"), std::string::npos) << r.error;
    EXPECT_NE(r.error.find("mirror"), std::string::npos) << r.error;
}

TEST(Config, PeriodBounds) {
    EXPECT_FALSE(parse_config("sync_period_sec=0\n").ok);
    EXPECT_FALSE(parse_config("sync_period_sec=3601\n").ok);
    EXPECT_TRUE(parse_config("sync_period_sec=3600\n").ok);
    EXPECT_FALSE(parse_config("world_save_sec=29\n").ok);
}

TEST(Config, WhitespaceTolerant) {
    auto r = parse_config("  host   =   a.b  \n");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.cfg.host, "a.b");
}
