cmake_minimum_required(VERSION 3.30)
project(dp_dbstorage CXX C)

# DP_DBStorage — graft-плагин: сохранение состояния сервера DayZ в MariaDB
# (полная декомпозиция построчно, план: docs в README). PBO-часть мода лежит в
# mod/DP_DBStorage_Server, упаковывает её deploy.ps1 (MakePbo).
#
# Сборка (deploy.ps1 делает то же из VS-окружения):
#   cmake --preset release -DGRAFT_SOURCE_DIR=C:/Grafted
#   cmake --build --preset release
# Тесты без игры:
#   cmake --preset tests -DGRAFT_SOURCE_DIR=C:/Grafted
#   cmake --build --preset tests && ctest --preset tests
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
    # Статический рантайм — как у graft (требование ABI плагинов).
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(GRAFT_BOOT_FILE "${CMAKE_BINARY_DIR}/graft.boot.cmake" CACHE FILEPATH "graft.boot.cmake")
if(NOT EXISTS "${GRAFT_BOOT_FILE}")
    file(DOWNLOAD https://raw.githubusercontent.com/KRdayzmodding/KR_GRAFTED/main/cmake/graft.boot.cmake "${GRAFT_BOOT_FILE}")
endif()
include("${GRAFT_BOOT_FILE}")
# Тег релиза, не main (правило CONTRIBUTING graft). Локальный клон подставляется
# через -DGRAFT_SOURCE_DIR, чтобы не качать при каждой конфигурации.
graft_import(graft https://github.com/KRdayzmodding/KR_GRAFTED TAG v0.4.0)

# ── MariaDB Connector/C (LGPL, статикой) ─────────────────────────────────────
# TLS — SCHANNEL (системный, OpenSSL не нужен); curl/тесты/инструменты — OFF.
# CMAKE_COMPILE_WARNING_AS_ERROR выключаем: субпроект собирается с /WX и под
# clang-cl сыплет предупреждениями, которые нам не решить. WIN32_LEAN_AND_MEAN
# их CMake даёт только в НЕ-субпроектной сборке — без него winsock.h
# конфликтует с winsock2.h (SCHANNEL-файлы).
include(FetchContent)
set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DWIN32_LEAN_AND_MEAN -DNOGDI")
FetchContent_Declare(mariadb
    URL https://github.com/mariadb-corporation/mariadb-connector-c/archive/refs/tags/v3.4.9.tar.gz)
set(WITH_SSL        ON  CACHE BOOL "" FORCE)   # SCHANNEL на Windows
set(WITH_CURL       OFF CACHE BOOL "" FORCE)
set(WITH_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_TOOLS      OFF CACHE BOOL "" FORCE)
set(WITH_MSI        OFF CACHE BOOL "" FORCE)
# Динамические плагины-модули ломают INSTALL субпроекта (no LIBRARY DESTINATION)
# и тянут DLL в рантайме — всё нужное собираем статикой, лишнее выключаем.
set(CLIENT_PLUGIN_DIALOG                OFF    CACHE STRING "" FORCE)
set(CLIENT_PLUGIN_PARSEC                OFF    CACHE STRING "" FORCE)
set(CLIENT_PLUGIN_AUTH_GSSAPI_CLIENT    OFF    CACHE STRING "" FORCE)
set(CLIENT_PLUGIN_PVIO_SHMEM            STATIC CACHE STRING "" FORCE)
set(CLIENT_PLUGIN_CLIENT_ED25519        STATIC CACHE STRING "" FORCE)
set(CLIENT_PLUGIN_CACHING_SHA2_PASSWORD STATIC CACHE STRING "" FORCE)
set(CLIENT_PLUGIN_SHA256_PASSWORD       STATIC CACHE STRING "" FORCE)
set(CLIENT_PLUGIN_MYSQL_CLEAR_PASSWORD  STATIC CACHE STRING "" FORCE)
FetchContent_MakeAvailable(mariadb)

# ── Плагин ───────────────────────────────────────────────────────────────────
# Объявления печатаются прямо в мод — PBO собирается из него же.
graft_plugin(dp_dbstorage
    NAME    DP_DBStorage
    VERSION 1
    SOURCES
        src/plugin.cpp
        src/natives.cpp
        src/boot.cpp
        src/worker.cpp
        src/store.cpp
        src/schema.cpp
        src/db.cpp
        src/db_mariadb.cpp
        src/sqlite.cpp
        src/validate.cpp
    MODULES 3_Game
    SCRIPTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/mod/DP_DBStorage_Server/scripts")
target_include_directories(dp_dbstorage PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# mariadbclient не экспортирует пути заголовков — даём сами: source include/
# (mysql.h) и binary include/ (сгенерированный mariadb_version.h).
target_include_directories(dp_dbstorage PRIVATE
    "${mariadb_SOURCE_DIR}/include" "${mariadb_BINARY_DIR}/include")
# Системные библиотеки коннектора (winsock и пр.) — их CMake расставляет по
# IMPORTED-цели, транзитивно это не приезжает, линкуем сами.
target_link_libraries(dp_dbstorage PRIVATE mariadbclient ws2_32 shlwapi crypt32 advapi32)

# ── Юнит-тесты без игры (gtest, только движко-независимые источники) ─────────
if(DPDB_BUILD_TESTS)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
        EXCLUDE_FROM_ALL)
    set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    foreach(t gtest gtest_main gmock gmock_main)
        if(TARGET ${t})
            target_compile_options(${t} PRIVATE -clang:-Wno-character-conversion)
        endif()
    endforeach()

    add_executable(dpdb_tests
        tests/config_test.cpp
        tests/validate_test.cpp
        tests/schema_test.cpp
        tests/store_test.cpp
        src/db.cpp
        src/validate.cpp
        src/schema.cpp
        src/store.cpp)
    target_include_directories(dpdb_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(dpdb_tests PRIVATE GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(dpdb_tests)

    add_custom_target(tests DEPENDS dpdb_tests)
    set_target_properties(dpdb_tests tests PROPERTIES FOLDER tests)
endif()
