// Валидация входов из Enforce и экранирование SQL-литералов.
// Нативы получают строки из скриптов — путь инъекции в SQL, поэтому всё
// проверяется до попадания в запрос (паттерн ValidUid/SqlQuote из DP_Storage).
#include "validate.hpp"

#include <gtest/gtest.h>

using namespace dpdb;

TEST(Validate, Uid) {
    EXPECT_TRUE(valid_uid("pP0aBcDeFgHiJkLmNoPqRsTuVwXyZ0123456789_=x-y"));
    EXPECT_FALSE(valid_uid(""));           // пусто
    EXPECT_FALSE(valid_uid("abc"));        // короче 5
    EXPECT_FALSE(valid_uid("abc def"));    // пробел
    const std::string long65(65, 'a');
    EXPECT_FALSE(valid_uid(long65));
    const std::string ok64(64, 'a');
    EXPECT_TRUE(valid_uid(ok64));
}

TEST(Validate, TypeName) {
    EXPECT_TRUE(valid_type_name("SodaCan_Cola"));
    EXPECT_TRUE(valid_type_name("b"));
    EXPECT_FALSE(valid_type_name(""));
    EXPECT_FALSE(valid_type_name("a b"));
    EXPECT_FALSE(valid_type_name("It's"));       // кавычка
    EXPECT_FALSE(valid_type_name("A.B"));        // точка — не бывает в имени типа
    EXPECT_FALSE(valid_type_name("A+B"));
    const std::string ok96(96, 'X');
    const std::string bad97(97, 'X');
    EXPECT_TRUE(valid_type_name(ok96));
    EXPECT_FALSE(valid_type_name(bad97));
}

TEST(Validate, Layer) {
    for (auto l : {"dynamic", "vehicles", "zombies", "animals", "buildings",
                   "stashes", "events"})
        EXPECT_TRUE(valid_layer(l)) << l;
    // types/doors — бухгалтерия CE, живыми сущностями не ходит (разведка DP)
    EXPECT_FALSE(valid_layer("types"));
    EXPECT_FALSE(valid_layer("doors"));
    EXPECT_FALSE(valid_layer("loot"));     // обобщение, не слой
    EXPECT_FALSE(valid_layer(""));
    EXPECT_FALSE(valid_layer("dynamic "));
}

TEST(Validate, Slot) {
    EXPECT_TRUE(valid_slot(""));           // корень без слота
    EXPECT_TRUE(valid_slot("Melee"));
    EXPECT_TRUE(valid_slot("Cargo 3 2"));  // cargo: строка/столбец
    EXPECT_FALSE(valid_slot("It's"));
    EXPECT_FALSE(valid_slot("a\tb"));
    const std::string ok64(64, 's');
    const std::string bad65(65, 's');
    EXPECT_TRUE(valid_slot(ok64));
    EXPECT_FALSE(valid_slot(bad65));
}

TEST(Validate, SqlQuote) {
    EXPECT_EQ(sql_quote("abc"), "'abc'");
    EXPECT_EQ(sql_quote(""), "''");
    EXPECT_EQ(sql_quote("a'b"), "'a''b'");
    // обратный слеш — escape-символ MySQL/MariaDB, удваивать обязательно:
    // '\n' в литерале без удвоения превратился бы в перевод строки
    EXPECT_EQ(sql_quote("a\\b"), "'a\\\\b'");
}

TEST(Validate, HexLiteral) {
    const std::uint8_t bytes[] = {0xAB, 0x01, 0x00, 0xFF};
    EXPECT_EQ(hex_literal(bytes, 4), "0xAB0100FF");
    EXPECT_EQ(hex_literal(nullptr, 0), "0x");
}

TEST(Validate, LayersWhitelistMatches) {
    const auto& all = layers();
    EXPECT_EQ(all.size(), size_t{7});
}
