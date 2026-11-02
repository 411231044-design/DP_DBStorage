// Точка старта плагина: GRAFT_ON_LOAD → пути → конфиг → takeover-чистка
// storage_1 (до чтения persistence движком) → фоновый воркер.
#pragma once

#include "db.hpp"
#include "worker.hpp"

namespace dpdb::boot {

void at_load();

// Включить streams-перенаправление, если ещё не включено. false — не вышло
// (причина уже в журнале и в worker_state().streams_note). Вызывается из
// ON_LOAD и — повторно — воркером, пока не активируется: MariaDB может
// подняться позже старта сервера (окно до первого чтения storage ~десятки
// секунд). Потокобезопасно: слоты IAT пишутся атомарно, гонка вызова видит
// либо старый, либо новый адрес.
bool ensure_streams_redirect(const Paths& paths, const DbConfig& cfg);

}  // namespace dpdb::boot
