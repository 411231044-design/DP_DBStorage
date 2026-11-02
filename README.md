# DP_DBStorage

Серверный graft-мод DayZ: сохранение состояния сервера в **MariaDB** — полная
декомпозиция построчно (игроки с инвентарём, сущности мира, машины), без
бинарного `storage_1`. Работает поверх [KR_GRAFTED](https://github.com/KRdayzmodding/KR_GRAFTED):
C++-плагин `DP_DBStorage.grafted.dll` вызывается из Enforce-скриптов как обычные
нативные функции. Без ядра GRAFT мод компилируется пустым (`#ifdef`) и ничему
не мешает.

Разведочная база и предшественник — DP_Storage (живые операции с persistence,
экономика CE): там же подтверждены паттерны «воркер-поток + флаг-файлы»,
решёточный обход карты и параллельный доступ к players.db.

## Архитектура

```
Enforce (мод)                     C++ плагин (graft native)         MariaDB
──────────────                    ─────────────────────────         ───────
DbPlayerSaver  ──нативы-батчи──►  staging → очередь (coalesce) ──► воркер ──► SQL
DbWorldSaver   (решётка+инвентарь)                                  (транзакции)
MissionServer  ◄──флаг-файлы─────  db_commands (вайпы/кики, фаза 3)
```

- Всё игровое — на сим-потоке; сеть/диск MariaDB — в фоновом воркере.
- Режим `takeover`: на `GRAFT_ON_LOAD` (до чтения persistence движком) слои
  `storage_1/data` и строки `players.db` стираются — истина только в MariaDB.
- Теневые BLOB (байты players.db / файлов слоёв) пишутся рядом — аварийный
  откат, если script-API не покрыл какое-то поле (см. docs/fidelity.md, фаза 1).

## Конфиг `$profile:dp_dbstorage.cfg`

```
host = 127.0.0.1
port = 3306
user = dpstorage
password = секрет
database = dp_storage
mode = takeover        # takeover | mirror
sync_period_sec = 30
player_save_sec = 60
world_save_sec = 300
```

## Сборка

Тулчейн: VS Build Tools 2022 (C++ workload), LLVM (clang-cl 17+), CMake 3.30+,
Ninja, Mikero MakePbo.

```powershell
powershell -File deploy.ps1                                    # деплой в @dbstorage
cmake --preset tests -DGRAFT_SOURCE_DIR=C:/Grafted             # gtest без игры
cmake --build --preset tests; ctest --preset tests
```

## Статус

- Фаза 0 (каркас): сборка, схема/миграции, воркер, теневые BLOB, takeover-чистка.
- Фаза 1: Enforce-обходчики (игрок, мир) — в работе.
- Фаза 2: восстановление из MariaDB на буте/коннекте.
- Фаза 3: db_commands (вайпы/кики/снапшоты).
