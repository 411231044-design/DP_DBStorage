// Фабрика реального клиента СУБД (реализация — db_mariadb.cpp, libmariadb).
#pragma once
#include <memory>
#include <string>

#include "db.hpp"

namespace dpdb {

std::unique_ptr<DbClient> make_mariadb_client(const DbConfig& cfg);
std::string mariadb_client_version();

}  // namespace dpdb
