// Реальный клиент СУБД: MariaDB Connector/C (libmariadb, статикой).
// Используется только фоновым воркером; нитей внутрь не пускаем — свои мьютексы
// на всякий случай есть (статус тянут нативы).
#include "db.hpp"

#include <charconv>
#include <mutex>
#include <utility>

#include <mysql.h>

namespace dpdb {

namespace {

Val cell_to_val(const char* text, unsigned long len, enum_field_types type,
                unsigned int flags) {
    if (!text)
        return {};   // SQL NULL
    const std::string_view s(text, len);
    // Числа парсим первыми: у числовых результатов MariaDB стоит BINARY_FLAG
    // (все данные приезжают байтами), это не признак BLOB-колонки.
    switch (type) {
        case MYSQL_TYPE_TINY:
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_LONGLONG:
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_YEAR: {
            std::int64_t v = 0;
            std::from_chars(s.data(), s.data() + s.size(), v);
            return v;
        }
        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE:
        case MYSQL_TYPE_DECIMAL:
        case MYSQL_TYPE_NEWDECIMAL: {
            double d = 0;
            std::from_chars(s.data(), s.data() + s.size(), d);
            return d;
        }
        default:
            break;
    }
    // Строковые типы: BINARY_FLAG отличает настоящие байты (BLOB/VARBINARY)
    // от текста (TEXT/VARCHAR) — у текста кодировка не бинарная.
    if (flags & BINARY_FLAG) {
        Blob b(s.begin(), s.end());
        return b;
    }
    return std::string(s);
}

class MariaDbClient final : public DbClient {
public:
    explicit MariaDbClient(DbConfig cfg) : cfg_(std::move(cfg)) {}

    ~MariaDbClient() override { close(); }

    bool connect() {
        std::lock_guard<std::mutex> lk(mx_);
        close_locked();
        mysql_ = mysql_init(nullptr);
        if (!mysql_) {
            error_ = "mysql_init: out of memory";
            return false;
        }
        mysql_options(mysql_, MYSQL_SET_CHARSET_NAME, "utf8mb4");
        bool reconnect_off = false;   // авто-reconnect устарел — чиним сами
        mysql_options(mysql_, MYSQL_OPT_RECONNECT, &reconnect_off);
        if (!mysql_real_connect(mysql_, cfg_.host.c_str(), cfg_.user.c_str(),
                                cfg_.password.empty()
                                    ? nullptr
                                    : cfg_.password.c_str(),
                                cfg_.database.c_str(), cfg_.port, nullptr, 0)) {
            error_ = std::string("connect: ") + mysql_error(mysql_);
            close_locked();
            return false;
        }
        connected_ = true;
        error_.clear();
        return true;
    }

    bool exec(const std::string& sql) override {
        std::lock_guard<std::mutex> lk(mx_);
        if (!mysql_)
            return fail("нет соединения");
        if (mysql_query(mysql_, sql.c_str()) != 0)
            return fail(mysql_error(mysql_));
        // результат (если вдруг SELECT) обязан быть вычитан до следующего запроса
        MYSQL_RES* res = mysql_store_result(mysql_);
        if (res)
            mysql_free_result(res);
        return true;
    }

    bool query(const std::string& sql, std::vector<DbRow>& out) override {
        std::lock_guard<std::mutex> lk(mx_);
        if (!mysql_)
            return fail("нет соединения");
        if (mysql_query(mysql_, sql.c_str()) != 0)
            return fail(mysql_error(mysql_));
        MYSQL_RES* res = mysql_store_result(mysql_);
        if (!res)
            return fail(mysql_error(mysql_));
        MYSQL_FIELD* fields = mysql_fetch_fields(res);
        const unsigned n = mysql_num_fields(res);
        while (MYSQL_ROW row = mysql_fetch_row(res)) {
            unsigned long* lens = mysql_fetch_lengths(res);
            out.emplace_back();
            for (unsigned i = 0; i < n; ++i)
                out.back().cols.push_back(
                    cell_to_val(row[i], lens ? lens[i] : 0, fields[i].type,
                                fields[i].flags));
        }
        mysql_free_result(res);
        return true;
    }

    bool connected() const override {
        std::lock_guard<std::mutex> lk(mx_);
        return connected_;
    }

    bool reconnect() override {
        close();
        return connect();
    }

    std::string error() const override {
        std::lock_guard<std::mutex> lk(mx_);
        return error_;
    }

    std::string server_info() const override {
        std::lock_guard<std::mutex> lk(mx_);
        return mysql_ ? mysql_get_server_info(mysql_) : std::string();
    }

private:
    bool fail(const std::string& e) {
        error_ = e;
        // соединение роняем только на сетевых ошибках; честнее — всегда
        // перевзять на reconnect() воркера
        connected_ = false;
        return false;
    }

    void close() {
        std::lock_guard<std::mutex> lk(mx_);
        close_locked();
    }

    void close_locked() {
        if (mysql_) {
            mysql_close(mysql_);
            mysql_ = nullptr;
        }
        connected_ = false;
    }

    DbConfig cfg_;
    mutable std::mutex mx_;
    MYSQL* mysql_ = nullptr;
    bool connected_ = false;
    std::string error_;
};

}  // namespace

std::unique_ptr<DbClient> make_mariadb_client(const DbConfig& cfg) {
    return std::make_unique<MariaDbClient>(cfg);
}

std::string mariadb_client_version() {
    return mysql_get_client_info();
}

}  // namespace dpdb
