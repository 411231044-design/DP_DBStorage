// Паспорт плагина и точки входа. Вся логика — в src/*.cpp, здесь только каркас:
// ON_LOAD — до чтения persistence движком (пути, конфиг, takeover, воркер).
#include "graft/native.hpp"

#include "boot.hpp"

GRAFT_PLUGIN("DP_DBStorage", 1);

GRAFT_ON_LOAD() {
    dpdb::boot::at_load();
}
