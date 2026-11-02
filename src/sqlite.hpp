// Системный winsqlite3.dll (есть в Windows 10/11) — грузим динамически, ноль
// вендоренного кода (паттерн DP_Storage). Нужен для players.db: takeover-вайп
// на буте и теневые BLOB персонажей.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dpdb::sqlite {

inline constexpr int kOk = 0, kRow = 100, kDone = 101;
inline constexpr int kOpenReadonly = 1, kOpenReadWrite = 2, kOpenCreate = 4;

bool available();
std::string version();

class Db;   // ниже — непрозрачная обёртка над sqlite3*

struct Cell {
    std::string text;                  // текстовая колонка
    std::vector<std::uint8_t> blob;    // бинарная колонка (если была)
    bool is_blob = false;
};

class Db {
public:
    Db() = default;
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;
    ~Db();

    // Открыть файл (флаги kOpen*). "" = ок, иначе текст ошибки.
    std::string open(const std::string& path, int flags);
    bool ok() const { return handle_ != nullptr; }

    // Один оператор без результата: "", либо ошибка.
    std::string exec(const std::string& sql);

    // Сколько строк затронул последний exec (DELETE/UPDATE/INSERT).
    int changes();

    // SELECT: строки из текстовых/blob колонок. false — ошибка (см. last_error).
    bool query(const std::string& sql, std::vector<std::vector<Cell>>& rows);

    const std::string& last_error() const { return last_error_; }

private:
    void close();

    void* handle_ = nullptr;   // sqlite3*: без unique_ptr — deleter в sqlite.cpp
    std::string last_error_;
};

}  // namespace dpdb::sqlite
