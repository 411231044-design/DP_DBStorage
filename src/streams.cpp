#include "streams.hpp"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <format>
#include <fstream>
#include <mutex>
#include <unordered_map>

#include "db.hpp"
#include "validate.hpp"

namespace dpdb {

// ── переписывание пути ───────────────────────────────────────────────────────

namespace {

bool is_sep(char c) {
    return c == '\\' || c == '/';
}

// in начинается с storage_dir (регистронезависимо, оба разделителя) и дальше
// есть остаток — заменяем префикс на cache_dir.
bool starts_with_dir(std::string_view in, std::string_view dir) {
    if (in.size() < dir.size())
        return false;
    for (std::size_t i = 0; i < dir.size(); ++i) {
        const char a = in[i], b = dir[i];
        if (a == b)
            continue;
        if ((is_sep(a) && is_sep(b)))
            continue;
        if (std::tolower(static_cast<unsigned char>(a)) !=
            std::tolower(static_cast<unsigned char>(b)))
            return false;
    }
    return true;
}

}  // namespace

bool redirect_path(std::string_view storage_dir, std::string_view cache_dir,
                   std::string_view in, std::string& out) {
    if (storage_dir.empty() || cache_dir.empty())
        return false;
    if (!starts_with_dir(in, storage_dir))
        return false;
    std::string_view rest = in.substr(storage_dir.size());
    if (!rest.empty() && !is_sep(rest.front()))
        return false;   // storage_1x — не наш каталог
    // хвост с любыми разделителями — нормализуем в '\\'
    std::string tail;
    tail.reserve(rest.size());
    for (const char c : rest)
        tail += is_sep(c) ? '\\' : c;
    out.assign(cache_dir).append(tail);
    return true;
}

// ── IAT-хук ──────────────────────────────────────────────────────────────────

namespace {

using FopenFn = FILE* (*)(const char*, const char*);

std::mutex g_redir_mx;
std::string g_storage_dir, g_cache_dir;
bool g_redirect_on = false;
std::atomic<std::uint64_t> g_redirect_count{0};
FopenFn g_real_fopen = nullptr;

// Журнал диагностики (подключает boot: log_line из воркера).
void (*g_diag_fn)(const std::string&) = nullptr;
std::atomic<std::uint64_t> g_fopen_logged{0};
std::atomic<std::uint64_t> g_cfw_logged{0};
std::atomic<std::uint64_t> g_cfw_storage_hits{0};

void log_diag(const std::string& s) {
    if (g_diag_fn)
        g_diag_fn(s);
}

// Слоты, которые мы запатчили (снять — вернуть оригинал).
struct PatchedSlot {
    void** slot;
    void* original;
};
std::vector<PatchedSlot> g_patched;

bool looks_like_persistence(const std::string& p);   // ниже, в секции диагностики

FILE* fopen_detour(const char* path, const char* mode) {
    if (path) {
        std::string redirected;
        bool hit = false;
        {
            std::lock_guard<std::mutex> lk(g_redir_mx);
            hit = g_redirect_on &&
                  redirect_path(g_storage_dir, g_cache_dir, path, redirected);
        }
        if (hit) {
            g_redirect_count.fetch_add(1, std::memory_order_relaxed);
            return g_real_fopen(redirected.c_str(), mode);
        }
        // диагностика: первые пути, похожие на persistence (могут быть
        // относительными — тогда redirect_path их не узнаёт)
        if (g_fopen_logged.load(std::memory_order_relaxed) < 24) {
            const std::string p = path;
            if (looks_like_persistence(p)) {
                g_fopen_logged.fetch_add(1, std::memory_order_relaxed);
                log_diag("fopen(пропущен): " + p + " mode=" +
                         (mode ? mode : "?"));
            }
        }
    }
    return g_real_fopen(path, mode);
}

// ── диагностика: какие API реально трогают persistence ──────────────────────

bool looks_like_persistence(const std::string& p) {
    const std::string low = [=] {
        std::string s;
        s.reserve(p.size());
        for (const char c : p)
            s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }();
    return low.find("storage_") != std::string::npos ||
           low.find("players.db") != std::string::npos ||
           low.find("dpdb_cache") != std::string::npos;
}

using CreateFileWFn = HANDLE (*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                 DWORD, DWORD, HANDLE);
CreateFileWFn g_real_CreateFileW = nullptr;

std::string wide_to_narrow(LPCWSTR w) {
    if (!w)
        return {};
    char buf[512];
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf),
                                      nullptr, nullptr);
    return n > 0 ? std::string(buf, n - 1) : std::string();
}

std::wstring narrow_to_wide(const std::string& s) {
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                      static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        w.data(), n);
    return w;
}

// Общий редирект для wide-путей: true → out готов (\\?\-префикс сохранён).
bool redirect_wide(LPCWSTR name, std::wstring& out) {
    if (!name || !g_redirect_on)
        return false;
    const std::string p = wide_to_narrow(name);
    if (!looks_like_persistence(p))
        return false;
    std::string_view pv = p;
    const bool extended = pv.starts_with("\\\\?\\");
    if (extended)
        pv.remove_prefix(4);
    std::string redirected;
    {
        std::lock_guard<std::mutex> lk(g_redir_mx);
        if (!redirect_path(g_storage_dir, g_cache_dir, pv, redirected))
            return false;
    }
    out = narrow_to_wide(extended ? "\\\\?\\" + redirected : redirected);
    return true;
}

HANDLE CreateFileW_detour(LPCWSTR name, DWORD access, DWORD share,
                          LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags,
                          HANDLE tmpl) {
    if (name) {
        const std::string p = wide_to_narrow(name);
        if (looks_like_persistence(p)) {
            g_cfw_storage_hits.fetch_add(1, std::memory_order_relaxed);
            if (g_cfw_logged.load(std::memory_order_relaxed) < 30) {
                g_cfw_logged.fetch_add(1, std::memory_order_relaxed);
                log_diag("CreateFileW: " + p);
            }
            // движок ходит в persistence через CreateFileW с \\?\-префиксом
            std::wstring w;
            if (redirect_wide(name, w)) {
                g_redirect_count.fetch_add(1, std::memory_order_relaxed);
                return g_real_CreateFileW(w.c_str(), access, share, sa, disp,
                                          flags, tmpl);
            }
        }
    }
    return g_real_CreateFileW(name, access, share, sa, disp, flags, tmpl);
}

// SQLite и слои живут всем файловым набором: журнал players.db удаляется
// DeleteFileW, temp-файлы слоёв переименовываются MoveFileExW, существование
// проверяется GetFileAttributesW. Редиректим весь набор — иначе кэш и
// настоящий путь расходятся и offlineDB падает с disk I/O error.
using DeleteFileWFn = BOOL (*)(LPCWSTR);
DeleteFileWFn g_real_DeleteFileW = nullptr;
using MoveFileExWFn = BOOL (*)(LPCWSTR, LPCWSTR, DWORD);
MoveFileExWFn g_real_MoveFileExW = nullptr;
using GetFileAttributesWFn = DWORD (*)(LPCWSTR);
GetFileAttributesWFn g_real_GetFileAttributesW = nullptr;
using SetFileAttributesWFn = BOOL (*)(LPCWSTR, DWORD);
SetFileAttributesWFn g_real_SetFileAttributesW = nullptr;
using CreateDirectoryWFn = BOOL (*)(LPCWSTR, LPSECURITY_ATTRIBUTES);
CreateDirectoryWFn g_real_CreateDirectoryW = nullptr;

BOOL DeleteFileW_detour(LPCWSTR name) {
    std::wstring w;
    if (redirect_wide(name, w)) {
        g_redirect_count.fetch_add(1, std::memory_order_relaxed);
        return g_real_DeleteFileW(w.c_str());
    }
    return g_real_DeleteFileW(name);
}

BOOL MoveFileExW_detour(LPCWSTR from, LPCWSTR to, DWORD flags) {
    std::wstring w1, w2;
    const bool h1 = redirect_wide(from, w1);
    const bool h2 = redirect_wide(to, w2);
    if (h1 || h2) {
        g_redirect_count.fetch_add(1, std::memory_order_relaxed);
        return g_real_MoveFileExW(h1 ? w1.c_str() : from, h2 ? w2.c_str() : to,
                                  flags);
    }
    return g_real_MoveFileExW(from, to, flags);
}

DWORD GetFileAttributesW_detour(LPCWSTR name) {
    std::wstring w;
    if (redirect_wide(name, w)) {
        g_redirect_count.fetch_add(1, std::memory_order_relaxed);
        return g_real_GetFileAttributesW(w.c_str());
    }
    return g_real_GetFileAttributesW(name);
}

BOOL SetFileAttributesW_detour(LPCWSTR name, DWORD attrs) {
    std::wstring w;
    if (redirect_wide(name, w)) {
        g_redirect_count.fetch_add(1, std::memory_order_relaxed);
        return g_real_SetFileAttributesW(w.c_str(), attrs);
    }
    return g_real_SetFileAttributesW(name, attrs);
}

BOOL CreateDirectoryW_detour(LPCWSTR name, LPSECURITY_ATTRIBUTES sa) {
    std::wstring w;
    if (redirect_wide(name, w)) {
        g_redirect_count.fetch_add(1, std::memory_order_relaxed);
        return g_real_CreateDirectoryW(w.c_str(), sa);
    }
    return g_real_CreateDirectoryW(name, sa);
}

// Импорты образа процесса: слоты с заданным именем.
std::vector<void**> import_slots(const char* func_name) {
    std::vector<void**> out;
    const auto base = reinterpret_cast<std::uint8_t*>(
        GetModuleHandleA(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return out;
    for (auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
                 base + dir.VirtualAddress);
         desc->Name; ++desc) {
        const auto* dll = reinterpret_cast<const char*>(base + desc->Name);
        if (!dll)
            continue;
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + (desc->OriginalFirstThunk
                        ? desc->OriginalFirstThunk
                        : desc->FirstThunk));
        auto** slot = reinterpret_cast<void**>(base + desc->FirstThunk);
        for (; thunk->u1.AddressOfData; ++thunk, ++slot) {
            if (IMAGE_SNAP_BY_ORDINAL(thunk->u1.Ordinal))
                continue;
            const auto* ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                base + thunk->u1.AddressOfData);
            if (ibn && std::string_view(ibn->Name) == func_name)
                out.push_back(slot);
        }
    }
    return out;
}

}  // namespace

void set_diag_logger(void (*fn)(const std::string&)) {
    g_diag_fn = fn;
}

void fopen_redirect_set(std::string storage_dir, std::string cache_dir) {
    std::lock_guard<std::mutex> lk(g_redir_mx);
    g_storage_dir = std::move(storage_dir);
    g_cache_dir = std::move(cache_dir);
    g_redirect_on = true;
}

bool fopen_redirect_active() {
    std::lock_guard<std::mutex> lk(g_redir_mx);
    return g_redirect_on;
}

std::uint64_t fopen_redirect_count() {
    return g_redirect_count.load(std::memory_order_relaxed);
}

std::uint64_t createfile_storage_hits() {
    return g_cfw_storage_hits.load(std::memory_order_relaxed);
}

int fopen_iat_install() {
    for (void** slot : import_slots("fopen")) {
        DWORD oldp = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldp))
            continue;
        if (g_real_fopen == nullptr)
            g_real_fopen = reinterpret_cast<FopenFn>(*slot);
        PatchedSlot ps{slot, *slot};
        *slot = reinterpret_cast<void*>(&fopen_detour);
        VirtualProtect(slot, sizeof(void*), oldp, &oldp);
        g_patched.push_back(ps);
    }
    // диагностика: подсчёт обращений к persistence через CreateFileW
    for (void** slot : import_slots("CreateFileW")) {
        DWORD oldp = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldp))
            continue;
        if (g_real_CreateFileW == nullptr)
            g_real_CreateFileW =
                reinterpret_cast<CreateFileWFn>(*slot);
        PatchedSlot ps{slot, *slot};
        *slot = reinterpret_cast<void*>(&CreateFileW_detour);
        VirtualProtect(slot, sizeof(void*), oldp, &oldp);
        g_patched.push_back(ps);
    }
    // полный файловый набор для прозрачности SQLite и temp+rename слоёв
    struct Aux {
        const char* name;
        void* detour;
        void** original;
    };
    Aux aux[] = {
        {"DeleteFileW", reinterpret_cast<void*>(&DeleteFileW_detour),
         reinterpret_cast<void**>(&g_real_DeleteFileW)},
        {"MoveFileExW", reinterpret_cast<void*>(&MoveFileExW_detour),
         reinterpret_cast<void**>(&g_real_MoveFileExW)},
        {"GetFileAttributesW", reinterpret_cast<void*>(&GetFileAttributesW_detour),
         reinterpret_cast<void**>(&g_real_GetFileAttributesW)},
        {"SetFileAttributesW",
         reinterpret_cast<void*>(&SetFileAttributesW_detour),
         reinterpret_cast<void**>(&g_real_SetFileAttributesW)},
        {"CreateDirectoryW",
         reinterpret_cast<void*>(&CreateDirectoryW_detour),
         reinterpret_cast<void**>(&g_real_CreateDirectoryW)},
    };
    for (const auto& a : aux)
        for (void** slot : import_slots(a.name)) {
            DWORD oldp = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldp))
                continue;
            if (*a.original == nullptr)
                *a.original = *slot;
            PatchedSlot ps{slot, *slot};
            *slot = a.detour;
            VirtualProtect(slot, sizeof(void*), oldp, &oldp);
            g_patched.push_back(ps);
        }
    return static_cast<int>(g_patched.size());
}

void fopen_iat_remove() {
    {
        std::lock_guard<std::mutex> lk(g_redir_mx);
        g_redirect_on = false;
    }
    for (const auto& ps : g_patched) {
        DWORD oldp = 0;
        if (VirtualProtect(ps.slot, sizeof(void*), PAGE_READWRITE, &oldp)) {
            *ps.slot = ps.original;
            VirtualProtect(ps.slot, sizeof(void*), oldp, &oldp);
        }
    }
    g_patched.clear();
}

// ── кэш ↔ СУБД ──────────────────────────────────────────────────────────────

namespace {

std::uint64_t fnv1a64(const std::uint8_t* p, std::size_t n) {
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

// Создать все каталоги пути (уже существующие — не ошибка).
void mkdirs(const std::string& dir) {
    std::string cur;
    cur.reserve(dir.size());
    for (std::size_t i = 0; i < dir.size(); ++i) {
        cur += dir[i];
        const bool sep = is_sep(dir[i]);
        const bool last = i + 1 == dir.size();
        if ((sep || last) && cur.size() > 3)
            CreateDirectoryA(cur.substr(0, cur.size() - (last && !sep ? 0 : 1))
                                 .c_str(),
                             nullptr);
    }
}

}  // namespace

// Чтение файла с полным share: движок держит слои открытыми, ifstream при
// чужом хэндле молча проваливается. false — файл недоступен.
bool read_shared(const std::string& path, Blob& out) {
    const std::wstring w = narrow_to_wide(path);
    HANDLE h = CreateFileW(w.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE |
                               FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    out.clear();
    char buf[65536];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0)
        out.insert(out.end(), buf, buf + n);
    CloseHandle(h);
    return true;
}

int cache_pull(DbClient& db, const std::string& cache_dir, std::string& err) {
    std::vector<DbRow> rows;
    if (!db.query("SELECT name, data FROM shadow_files", rows)) {
        err = db.error();
        return -1;
    }
    mkdirs(cache_dir + "\\data");
    int written = 0;
    for (const auto& r : rows) {
        const auto* name = std::get_if<std::string>(&r.cols[0]);
        const auto* data = std::get_if<Blob>(&r.cols[1]);
        if (!name || !data || name->empty() || data->empty())
            continue;
        // имена относительны кэшу и могут нести подкаталог (data\dynamic_000.bin).
        // Пишем только ОТСУТСТВУЮЩЕЕ: пережившее рестарт содержимое кэша —
        // самый свежий источник (движок мог сохранить на выключении позже
        // нашего пуша); MySQL закрывает только дыры.
        std::string rel = *name;
        for (char& c : rel)
            if (is_sep(c))
                c = '\\';
        const std::string path = cache_dir + "\\" + rel;
        if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES)
            continue;
        const auto cut = path.find_last_of('\\');
        if (cut != std::string::npos)
            mkdirs(path.substr(0, cut));
        std::ofstream f(path, std::ios::binary);
        if (!f)
            continue;
        f.write(reinterpret_cast<const char*>(data->data()),
                static_cast<std::streamsize>(data->size()));
        ++written;
    }
    return written;
}

void cache_push(DbClient& db, const std::string& cache_dir) {
    // текущие: имя → hash
    std::unordered_map<std::string, std::uint64_t> known;
    {
        std::vector<DbRow> rows;
        if (!db.query("SELECT name, hash FROM shadow_files", rows))
            return;   // СУБД недоступна — на следующий цикл
        for (const auto& r : rows)
            if (r.cols.size() >= 2) {
                const auto* n = std::get_if<std::string>(&r.cols[0]);
                const auto* h = std::get_if<std::int64_t>(&r.cols[1]);
                if (n && h)
                    known.emplace(*n, static_cast<std::uint64_t>(*h));
            }
    }
    // обход кэша рекурсивно (плоско: кэш\data\*.bin)
    WIN32_FIND_DATAA fd{};
    const HANDLE h =
        FindFirstFileA((cache_dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    std::vector<std::string> subdirs, files;
    do {
        const std::string_view n = fd.cFileName;
        if (n == "." || n == "..")
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            subdirs.emplace_back(fd.cFileName);
        else
            files.emplace_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    for (const auto& sub : subdirs) {
        const std::string pattern = cache_dir + "\\" + sub + "\\*";
        const HANDLE h2 = FindFirstFileA(pattern.c_str(), &fd);
        if (h2 == INVALID_HANDLE_VALUE)
            continue;
        do {
            const std::string_view n = fd.cFileName;
            if (n == "." || n == ".." || (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                continue;
            files.push_back(sub + "\\" + std::string(fd.cFileName));
        } while (FindNextFileA(h2, &fd));
        FindClose(h2);
    }

    for (const auto& rel : files) {
        Blob bytes;
        if (!read_shared(cache_dir + "\\" + rel, bytes))
            continue;   // занят двиглом без share — на следующем цикле
        const std::uint64_t hashv = fnv1a64(bytes.data(), bytes.size());
        if (const auto it = known.find(rel); it != known.end() && it->second == hashv)
            continue;
        db.exec(std::format(
            "REPLACE INTO shadow_files(name, data, bytes, hash, updated)"
            " VALUES ({}, {}, {}, {}, NOW())",
            sql_quote(rel), hex_literal(bytes.data(), bytes.size()),
            bytes.size(), static_cast<std::int64_t>(hashv)));
    }
}

void file_push(DbClient& db, const std::string& path, const std::string& name) {
    Blob bytes;
    if (!read_shared(path, bytes))
        return;
    const std::uint64_t hashv = fnv1a64(bytes.data(), bytes.size());
    std::vector<DbRow> rows;
    if (db.query(std::format("SELECT hash FROM shadow_files WHERE name = {}",
                             sql_quote(name)),
                 rows))
        if (!rows.empty() && rows.front().cols.size() == 1)
            if (const auto* h = std::get_if<std::int64_t>(&rows.front().cols[0]);
                h && static_cast<std::uint64_t>(*h) == hashv)
                return;   // не изменился
    db.exec(std::format(
        "REPLACE INTO shadow_files(name, data, bytes, hash, updated)"
        " VALUES ({}, {}, {}, {}, NOW())",
        sql_quote(name), hex_literal(bytes.data(), bytes.size()), bytes.size(),
        static_cast<std::int64_t>(hashv)));
}

}  // namespace dpdb
