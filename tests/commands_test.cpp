// db_commands: опрос и исполнение. players.db — настоящая SQLite-фикстура
// через winsqlite (обёртка плагина), СУБД-сторона — FakeDb.
#include <commands.hpp>

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>

#include "fake_db.hpp"
#include "sqlite.hpp"

namespace {

// Фикстура: живой players.db с двумя игроками. TempDir() кончается разделителем.
std::string make_players_db(const std::string& dir) {
    const std::string path = dir + "players_test.db";
    remove(path.c_str());
    dpdb::sqlite::Db db;
    std::string err = db.open(path, dpdb::sqlite::kOpenReadWrite |
                                        dpdb::sqlite::kOpenCreate);
    if (!err.empty()) {
        // winsqlite недоступен — тесты вайпа пропускаем пустым путём
        return {};
    }
    db.exec("CREATE TABLE Players(Id INTEGER PRIMARY KEY, Alive INT,"
            " UID CHAR(64) UNIQUE, Data BLOB)");
    db.exec("INSERT INTO Players(Alive, UID, Data) VALUES(1,"
            " 'pP0uid_test_1234', x'0102')");
    db.exec("INSERT INTO Players(Alive, UID, Data) VALUES(0,"
            " 'pP0other_uid_5678', x'0304')");
    return path;
}

dpdb::DbRow cmd_row(std::int64_t id, const char* cmd, const char* params) {
    dpdb::DbRow r;
    r.cols.emplace_back(id);
    r.cols.emplace_back(std::string(cmd));
    r.cols.emplace_back(std::string(params));
    return r;
}

}  // namespace

TEST(Commands, WipePlayerDeletesRowAndKicks) {
    const std::string dir = ::testing::TempDir();
    const std::string db_path = make_players_db(dir);
    const std::string kick = dir + "dp_db_kick.txt";
    remove(kick.c_str());

    dpdb::test::FakeDb fdb;
    fdb.replies.push_back({cmd_row(7, "wipe_player", "pP0uid_test_1234")});
    fdb.replies.push_back({});   // цикл опроса после исполнения — пусто

    dpdb::CommandEnv env{"streams", db_path, dir};
    dpdb::poll_commands(fdb, env);

    // строка исполнена и закрыта
    bool done = false;
    for (const auto& q : fdb.log)
        if (q.find("UPDATE db_commands SET status = 'done'") !=
                std::string::npos &&
            q.find("WHERE id = 7") != std::string::npos)
            done = true;
    EXPECT_TRUE(done);

    // игрок удалён из живой базы, второй жив
    dpdb::sqlite::Db check;
    ASSERT_EQ(check.open(db_path, dpdb::sqlite::kOpenReadonly), "");
    std::vector<std::vector<dpdb::sqlite::Cell>> rows;
    ASSERT_TRUE(check.query("SELECT UID FROM Players ORDER BY UID", rows));
    ASSERT_EQ(rows.size(), size_t{1});
    EXPECT_EQ(rows[0][0].text, "pP0other_uid_5678");

    // кик-флаг записан с uid
    std::ifstream f(kick);
    ASSERT_TRUE(f.good());
    std::string line;
    std::getline(f, line);
    EXPECT_EQ(line, "pP0uid_test_1234");
}

TEST(Commands, WipeAllPlayers) {
    const std::string dir = ::testing::TempDir();
    const std::string db_path = make_players_db(dir);
    const std::string kick = dir + "dp_db_kick.txt";
    remove(kick.c_str());

    dpdb::test::FakeDb fdb;
    fdb.replies.push_back({cmd_row(9, "wipe_all_players", "")});
    fdb.replies.push_back({});

    dpdb::CommandEnv env{"streams", db_path, dir};
    dpdb::poll_commands(fdb, env);

    dpdb::sqlite::Db check;
    ASSERT_EQ(check.open(db_path, dpdb::sqlite::kOpenReadonly), "");
    std::vector<std::vector<dpdb::sqlite::Cell>> rows;
    ASSERT_TRUE(check.query("SELECT UID FROM Players", rows));
    EXPECT_EQ(rows.size(), size_t{0});

    std::ifstream f(kick);
    std::string line;
    std::getline(f, line);
    EXPECT_EQ(line, "*");
}

TEST(Commands, BadUidAndUnknown) {
    dpdb::test::FakeDb fdb;
    fdb.replies.push_back({cmd_row(1, "wipe_player", "ab")});   // короткий uid
    fdb.replies.push_back({cmd_row(2, "rebase_mainframe", "")});
    fdb.replies.push_back({});

    dpdb::CommandEnv env{"streams", "", ::testing::TempDir()};
    dpdb::poll_commands(fdb, env);

    int errors = 0;
    for (const auto& q : fdb.log)
        if (q.find("status = 'error'") != std::string::npos)
            ++errors;
    EXPECT_EQ(errors, 2);
}

TEST(Commands, SnapshotCopiesStreams) {
    dpdb::test::FakeDb fdb;
    fdb.replies.push_back({cmd_row(5, "snapshot", "перед вайпом")});
    dpdb::DbRow snap_id;
    snap_id.cols.emplace_back(std::int64_t{42});
    fdb.replies.push_back({snap_id});        // LAST_INSERT_ID
    fdb.replies.push_back({});               // следующий опрос — пусто

    dpdb::CommandEnv env{"streams", "", ""};
    dpdb::poll_commands(fdb, env);

    bool copied = false, noted = false;
    for (const auto& q : fdb.log) {
        if (q.find("INSERT INTO snapshots_files") != std::string::npos &&
            q.find("SELECT 42, name, data, bytes, hash FROM shadow_files") !=
                std::string::npos)
            copied = true;
        if (q.find("INSERT INTO snapshots") != std::string::npos)
            noted = true;
    }
    EXPECT_TRUE(copied);
    EXPECT_TRUE(noted);
}
