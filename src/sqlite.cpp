#include "sqlite.hpp"

#include <windows.h>

namespace dpdb::sqlite {

namespace {

// Указатели на системный SQLite. 15 функций — весь нужный нам API.
struct Api {
    int (*open_v2)(const char*, void**, int, const char*);
    int (*close)(void*);
    const char* (*errmsg)(void*);
    int (*prepare_v2)(void*, const char*, int, void**, const char**);
    int (*step)(void*);
    int (*finalize)(void*);
    int (*exec)(void*, const char*, int (*)(void*, int, char**, char**), void*,
                char**);
    int (*changes)(void*);
    int (*busy_timeout)(void*, int);
    const char* (*libversion)();
    void (*free_)(void*);
    const unsigned char* (*column_text)(void*, int);
    const void* (*column_blob)(void*, int);
    int (*column_bytes)(void*, int);
    int (*column_type)(void*, int);
    int (*column_count)(void*);

    bool ok = false;
};

Api& api() {
    static Api a = [] {
        Api r{};
        HMODULE m = LoadLibraryW(L"winsqlite3.dll");
        if (!m)
            return r;
        const auto get = [&r, m](const char* name) -> void* {
            return reinterpret_cast<void*>(GetProcAddress(m, name));
        };
        r.open_v2 = reinterpret_cast<int (*)(const char*, void**, int,
                                             const char*)>(get("sqlite3_open_v2"));
        r.close = reinterpret_cast<int (*)(void*)>(get("sqlite3_close"));
        r.errmsg = reinterpret_cast<const char* (*)(void*)>(get("sqlite3_errmsg"));
        r.prepare_v2 =
            reinterpret_cast<int (*)(void*, const char*, int, void**,
                                     const char**)>(get("sqlite3_prepare_v2"));
        r.step = reinterpret_cast<int (*)(void*)>(get("sqlite3_step"));
        r.finalize = reinterpret_cast<int (*)(void*)>(get("sqlite3_finalize"));
        r.exec = reinterpret_cast<int (*)(void*, const char*,
                                          int (*)(void*, int, char**, char**),
                                          void*, char**)>(get("sqlite3_exec"));
        r.changes = reinterpret_cast<int (*)(void*)>(get("sqlite3_changes"));
        r.busy_timeout = reinterpret_cast<int (*)(void*, int)>(
            get("sqlite3_busy_timeout"));
        r.libversion =
            reinterpret_cast<const char* (*)()>(get("sqlite3_libversion"));
        r.free_ = reinterpret_cast<void (*)(void*)>(get("sqlite3_free"));
        r.column_text = reinterpret_cast<const unsigned char* (*)(void*, int)>(
            get("sqlite3_column_text"));
        r.column_blob =
            reinterpret_cast<const void* (*)(void*, int)>(get("sqlite3_column_blob"));
        r.column_bytes =
            reinterpret_cast<int (*)(void*, int)>(get("sqlite3_column_bytes"));
        r.column_type = reinterpret_cast<int (*)(void*, int)>(get("sqlite3_column_type"));
        r.column_count =
            reinterpret_cast<int (*)(void*)>(get("sqlite3_column_count"));
        r.ok = r.open_v2 && r.close && r.errmsg && r.prepare_v2 && r.step &&
               r.finalize && r.exec && r.changes && r.busy_timeout &&
               r.libversion && r.free_ && r.column_text && r.column_blob &&
               r.column_bytes && r.column_type && r.column_count;
        return r;
    }();
    return a;
}

constexpr int kText = 3, kBlob = 4;

}  // namespace

bool available() {
    return api().ok;
}

std::string version() {
    return api().ok ? api().libversion() : "winsqlite3.dll недоступен";
}

Db::~Db() {
    close();
}

void Db::close() {
    if (handle_ && api().ok)
        api().close(handle_);
    handle_ = nullptr;
}

std::string Db::open(const std::string& path, int flags) {
    if (!api().ok)
        return "winsqlite3.dll недоступен";
    void* h = nullptr;
    if (api().open_v2(path.c_str(), &h, flags, nullptr) != kOk) {
        const std::string err = h ? api().errmsg(h) : "open failed";
        if (h)
            api().close(h);
        return std::string("open: ") + err + " (" + path + ")";
    }
    api().busy_timeout(h, 1500);
    handle_ = h;
    return "";
}

std::string Db::exec(const std::string& sql) {
    if (!ok())
        return "база не открыта";
    char* err = nullptr;
    if (api().exec(handle_, sql.c_str(), nullptr, nullptr, &err) != kOk) {
        const std::string msg = err ? err : "exec failed";
        if (err)
            api().free_(err);
        last_error_ = msg;
        return msg;
    }
    return "";
}

bool Db::query(const std::string& sql, std::vector<std::vector<Cell>>& rows) {
    if (!ok()) {
        last_error_ = "база не открыта";
        return false;
    }
    void* stmt = nullptr;
    if (api().prepare_v2(handle_, sql.c_str(), -1, &stmt, nullptr) != kOk) {
        last_error_ = api().errmsg(handle_);
        return false;
    }
    for (;;) {
        const int rc = api().step(stmt);
        if (rc == kDone)
            break;
        if (rc != kRow) {
            last_error_ = api().errmsg(handle_);
            api().finalize(stmt);
            return false;
        }
        rows.emplace_back();
        auto& row = rows.back();
        const int width = api().column_count(stmt);
        for (int col = 0; col < width; ++col) {
            Cell c;
            if (api().column_type(stmt, col) == kBlob) {
                c.is_blob = true;
                const void* blob = api().column_blob(stmt, col);
                const int bytes = api().column_bytes(stmt, col);
                if (blob && bytes > 0)
                    c.blob.assign(static_cast<const std::uint8_t*>(blob),
                                  static_cast<const std::uint8_t*>(blob) + bytes);
            } else {
                const unsigned char* t = api().column_text(stmt, col);
                c.text = t ? reinterpret_cast<const char*>(t) : "";
            }
            row.push_back(std::move(c));
        }
    }
    api().finalize(stmt);
    return true;
}

}  // namespace dpdb::sqlite
