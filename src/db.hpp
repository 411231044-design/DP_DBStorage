// Тонкий слой над СУБД: значение SQL, конфиг, интерфейс клиента.
// Движко-независимая часть плагина — компилируется и в gtest без игры.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace dpdb {

// Значение SQL: NULL / целое / дробное / строка / байты.
using Blob = std::vector<std::uint8_t>;
using Val = std::variant<std::monostate, std::int64_t, double, std::string, Blob>;

// Литерал для текстового SQL по правилам MariaDB (экранирование внутри).
std::string sql_literal(const Val& v);

struct DbConfig {
    std::string host = "127.0.0.1";
    int port = 3306;
    std::string user = "dpstorage";
    std::string password;
    std::string database = "dp_storage";
    std::string mode = "streams";    // streams | takeover | mirror: streams —
                                     // IAT-перенаправление persistence в СУБД
    int sync_period_sec = 30;        // период воркера: тени/команды/сердцебиение
    int player_save_sec = 60;        // периодика снимка онлайн-игроков (скрипт)
    int world_save_sec = 300;        // периодика снимка мира (скрипт)
};

struct ConfigResult {
    DbConfig cfg;
    bool ok = false;
    std::string error;
};

// Парсер «ключ = значение»: '#' — комментарий до конца строки, BOM tolerated,
// неизвестный ключ — ошибка (опечатка не должна молча игнорироваться).
ConfigResult parse_config(std::string_view text);

struct DbRow {
    std::vector<Val> cols;
};

// Клиент СУБД — шов между плагином и MariaDB (в тестах — заглушка).
// Один оператор на вызов: много-операторные запросы сознательно не включаем
// (CLIENT_MULTI_STATEMENTS не запрашиваем).
class DbClient {
public:
    virtual ~DbClient() = default;
    virtual bool exec(const std::string& sql) = 0;                    // без результата
    virtual bool query(const std::string& sql, std::vector<DbRow>& out) = 0;
    virtual bool connected() const = 0;
    virtual bool reconnect() = 0;         // переподключение с нуля
    virtual std::string error() const = 0;
    virtual std::string server_info() const = 0;   // версия сервера, "" если нет
};

}  // namespace dpdb
