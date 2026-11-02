# CONTRIBUTING — DP_DBStorage

Работаем через PR, как в [KR_GRAFTED](https://github.com/KRdayzmodding/KR_GRAFTED):
`main` стабильный, прямой push закрыт, каждая правка — отдельной веткой и PR.

## Цикл

1. Ветка от `main`: `feat|fix|docs|test|refactor/<тема>` (например
   `feat/wipe-layer-command`). Долгоживущих веток нет — влилась и удалена.
2. PR = одна мысль. Заголовок PR становится текстом squash-коммита.
3. Формат коммитов — [Conventional Commits](https://www.conventionalcommits.org)
   (`feat:`, `fix:`, `docs:`, `test:`, `refactor:`), описание на русском.
4. Тесты вперёд кода: новая логика без кейса в `tests/` не принимается.
   Если проверяемо без игры — проверяется без игры (gtest, `FakeDb`).

## Проверка перед PR

```powershell
cmake --preset tests -DGRAFT_SOURCE_DIR=C:/Grafted   # gtest без игры
cmake --build --preset tests
cd build\tests && ctest -C Debug

powershell -File deploy.ps1                           # сборка + PBO + деплой
```

Требуется: VS Build Tools 2022 (C++), LLVM (clang-cl 17+), CMake 3.30+, Ninja
(`py -3.12 -m pip install cmake ninja`), Mikero MakePbo. Ядро graft ставится
один раз: `graft install <каталог сервера>`.

## Правила

- Пароли СУБД — только в `$profile:dp_dbstorage.cfg` на сервере. В репозиторий
  секреты не попадают (в примерах — плейсхолдеры).
- Байты движка не парсим «по памяти»: факты о движке — с пометкой сборки и
  перепроверкой (см. `tools/disannot.py` и RESEARCH KR_GRAFTED).
- Схема MariaDB меняется только новыми миграциями (`src/schema.cpp`,
  `kSchemaVersion` + 1), существующие не правятся.
- E2E-стенд: тестовая миссия-копия и отдельный `instanceId` — прод-storage
  не трогаем.

## Структура

См. README: `streams` — перехват файлового слоя persistence движка (IAT →
кэш ↔ MariaDB), `takeover` — скриптовая построчная декомпозиция. Новичку
для ориентира: `src/streams.cpp` (редирект), `src/worker.cpp` (воркер-цикл),
`mod/DP_DBStorage_Server/scripts/5_Mission/db_server.c` (Enforce-сторона).
