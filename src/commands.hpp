// Управление из любого mysql-клиента: таблица db_commands (строку вставили —
// плагин исполнил за цикл воркера, статус/результат вернулись в ту же строку).
// Воркер — единственный писатель; движок не трогается (вайп игрока — хирургия
// players.db, кик онлайн — флаг-файл для скрипта, паттерн DP_Storage).
#pragma once
#include <string>

#include "db.hpp"

namespace dpdb {

struct CommandEnv {
    std::string mode;               // streams | takeover | mirror
    std::string players_db;         // живой players.db (кэш в streams)
    std::string profiles_dir;       // сюда пишется флаг кика dp_db_kick.txt
};

// Один цикл опроса: все новые команды по порядку. Сетевые ошибки не роняют
// цикл — строка останется new и повторится.
void poll_commands(DbClient& db, const CommandEnv& env);

}  // namespace dpdb
