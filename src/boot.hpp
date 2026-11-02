// Точка старта плагина: GRAFT_ON_LOAD → пути → конфиг → takeover-чистка
// storage_1 (до чтения persistence движком) → фоновый воркер.
#pragma once

namespace dpdb::boot {

void at_load();

}  // namespace dpdb::boot
