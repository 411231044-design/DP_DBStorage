// Валидация входов из Enforce и экранирование SQL-литералов. Нативы получают
// строки из скриптов — это путь инъекции в SQL, поэтому всё проверяется до
// попадания в запрос (паттерн ValidUid/SqlQuote из DP_Storage).
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dpdb {

// UID игрока (DayZ identity): 5..64 символа [A-Za-z0-9=_-] — как ValidUid в DP.
bool valid_uid(std::string_view s);

// Имя типа Enforce: 1..96 символов [A-Za-z0-9_].
bool valid_type_name(std::string_view s);

// Слой мира: только белый список живых слоёв (types/doors — бухгалтерия CE).
bool valid_layer(std::string_view s);

// Слот инвентаря: 0..64 печатных символов без кавычек и управляющих.
bool valid_slot(std::string_view s);

// Произвольный текст (ник, extra): длина и отсутствие управляющих символов.
bool valid_text(std::string_view s, std::size_t max_len);

const std::vector<std::string_view>& layers();

// '…' с удвоением одинарных кавычек и обратных слешей (escape-символ MariaDB).
std::string sql_quote(std::string_view s);

// 0xABCD… — бинарный литерал MariaDB (безопасен для любых байтов).
std::string hex_literal(const std::uint8_t* p, std::size_t n);

}  // namespace dpdb
