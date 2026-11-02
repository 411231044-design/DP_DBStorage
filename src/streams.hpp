// Перехват файлового слоя persistence: IAT-слот fopen движка перенаправляет
// пути storage_<id> в одноразовый кэш, кэш синхронизируется с MariaDB.
// Движок остаётся полностью нативным (CE-счётчики, «существующий мир»,
// players.db), а единственное долговечное хранилище — СУБД.
//
// Режим config.mode == "streams". При недоступной СУБД на старте — отказ от
// перенаправления (движок работает с настоящим storage как раньше).
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dpdb {

// Переписать путь под кэш: storageDir\data/... → cacheDir\data/...
// Возвращает false, если путь не под persistence-каталогом. Чистая функция.
bool redirect_path(std::string_view storage_dir, std::string_view cache_dir,
                   std::string_view in, std::string& out);

// IAT-хук на fopen образа процесса: все слоты с именем "fopen" ведут на нас.
// Возвращает число запатченных слотов; при 0 перенаправление не работает.
int  fopen_iat_install();
void fopen_iat_remove();

// Настройка хука (до install): какие пути и куда переписывать.
void fopen_redirect_set(std::string storage_dir, std::string cache_dir);
bool fopen_redirect_active();

// Сколько перенаправлений случилось (диагностика).
std::uint64_t fopen_redirect_count();

// Сколько CreateFileW-обращений пришлось на persistence-пути (диагностика:
// если слои идут через Win32, а не CRT — это число растёт при нулевом
// fopen_redirect_count).
std::uint64_t createfile_storage_hits();

// Журнал диагностики путей (boot подключает сюда log_line воркера).
void set_diag_logger(void (*fn)(const std::string&));

// Кэш: вытащить все потоки из СУБД в кэш-каталог (бут, синхронно).
// Возвращает число записанных файлов; при ошибке СУБД — -1.
int cache_pull(class DbClient& db, const std::string& cache_dir,
               std::string& err);

// Кэш: отправить новые/изменённые файлы в СУБД (воркер, цикл).
void cache_push(class DbClient& db, const std::string& cache_dir);

// Один файл в shadow_files под именем name (играет и хеш-дедуп): players.db
// хранится целиком — байто-точное восстановление на буте.
void file_push(class DbClient& db, const std::string& path,
               const std::string& name);

}  // namespace dpdb
