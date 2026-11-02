// Перенаправление путей persistence (IAT-режим streams): чистая функция
// переписывания пути — без Win32, формат-ориентированные случаи.
#include <streams.hpp>

#include <gtest/gtest.h>

using dpdb::redirect_path;

namespace {
const char* kStorage = "C:\\srv\\mpmissions\\dayz.chernarusplus\\storage_2";
const char* kCache = "C:\\srv\\profiles_dpdb\\dpdb_cache";
}  // namespace

TEST(Streams, RedirectsDataFiles) {
    std::string out;
    EXPECT_TRUE(redirect_path(kStorage, kCache,
                              "C:\\srv\\mpmissions\\dayz.chernarusplus\\"
                              "storage_2\\data\\dynamic_000.bin",
                              out));
    EXPECT_EQ(out,
              "C:\\srv\\profiles_dpdb\\dpdb_cache\\data\\dynamic_000.bin");
    EXPECT_TRUE(redirect_path(kStorage, kCache,
                              "C:/srv/mpmissions/dayz.chernarusplus/"
                              "storage_2/data/vehicles.bin",
                              out));
    EXPECT_EQ(out, "C:\\srv\\profiles_dpdb\\dpdb_cache\\data\\vehicles.bin");
}

TEST(Streams, CaseAndSeparatorTolerant) {
    std::string out;
    EXPECT_TRUE(redirect_path(kStorage, kCache,
                              "c:\\SRV\\mpmissions\\dayz.chernarusplus\\"
                              "STORAGE_2\\data\\doors.bin",
                              out));
    EXPECT_EQ(out, "C:\\srv\\profiles_dpdb\\dpdb_cache\\data\\doors.bin");
}

TEST(Streams, ForeignPathsUntouched) {
    std::string out;
    // storage_2x — другой каталог (префикс совпал, но дальше не разделитель)
    EXPECT_FALSE(redirect_path(kStorage, kCache,
                               "C:\\srv\\mpmissions\\dayz.chernarusplus\\"
                               "storage_2x\\data\\a.bin",
                               out));
    EXPECT_FALSE(redirect_path(kStorage, kCache,
                               "C:\\srv\\profiles\\something.cfg", out));
    EXPECT_FALSE(redirect_path(kStorage, kCache, "storage_2", out));
    EXPECT_FALSE(redirect_path("", kCache, "C:\\any", out));
    EXPECT_FALSE(redirect_path(kStorage, "", "C:\\any", out));
}

TEST(Streams, RootFileRedirects) {
    std::string out;
    // файл прямо в storage_2 (без data\) — тоже наш, кэш корректен
    EXPECT_TRUE(redirect_path(kStorage, kCache,
                              "C:\\srv\\mpmissions\\dayz.chernarusplus\\"
                              "storage_2\\players.db",
                              out));
    EXPECT_EQ(out, "C:\\srv\\profiles_dpdb\\dpdb_cache\\players.db");
}
