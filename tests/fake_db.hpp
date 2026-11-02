// Запоминающая заглушка СУБД для gtest: пишет все операторы, отвечает
// запрограммированными строками или ошибками. Игра и MariaDB не нужны.
#pragma once
#include "db.hpp"

#include <deque>
#include <utility>
#include <vector>

namespace dpdb::test {

class FakeDb final : public dpdb::DbClient {
public:
    std::vector<std::string> log;                   // все операторы по порядку
    std::deque<std::string> errors;                 // будущие ошибки, по одной на вызов
    std::deque<std::vector<dpdb::DbRow>> replies;   // будущие ответы query, по одному

    bool exec(const std::string& sql) override {
        log.push_back(sql);
        if (!errors.empty()) {
            last_error_ = take(errors);
            return false;
        }
        return true;
    }

    bool query(const std::string& sql, std::vector<dpdb::DbRow>& out) override {
        log.push_back(sql);
        if (!errors.empty()) {
            last_error_ = take(errors);
            return false;
        }
        if (!replies.empty())
            out = take(replies);
        return true;
    }

    bool connected() const override { return connected_; }
    bool reconnect() override {
        connected_ = true;
        return true;
    }
    std::string error() const override { return last_error_; }
    std::string server_info() const override { return "FakeDb 11.4-DP"; }

    bool connected_ = true;

    // Сколько операторов в журнале начинается с префикса (подсчёт INSERT-пачек).
    size_t count_prefix(const char* prefix) const {
        size_t n = 0;
        for (const auto& s : log)
            if (s.rfind(prefix, 0) == 0)
                ++n;
        return n;
    }

private:
    std::string last_error_;
    template <class T>
    T take(std::deque<T>& d) {
        T v = std::move(d.front());
        d.pop_front();
        return v;
    }
};

}  // namespace dpdb::test
