// Версионированные миграции схемы dp_storage (MariaDB, InnoDB, utf8mb4).
#pragma once
#include <string>
#include <vector>

#include "db.hpp"

namespace dpdb {

inline constexpr int kSchemaVersion = 3;

struct Migration {
    int version;
    const char* name;
    std::vector<std::string> statements;
};

const std::vector<Migration>& migrations();

// Накатить недостающие миграции. Возвращает false при недоступной СУБД или
// ошибке оператора (откат не нужен: каждый оператор атомарен, а версия
// записывается только после успешного прохода).
bool apply_migrations(DbClient& db, std::string& log);

}  // namespace dpdb
