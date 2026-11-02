// СГЕНЕРИРОВАНО protogen из C++ реестра нативов — руками не править.
// Источник истины — блоки GRAFT_BINDINGS в исходниках мода.
// Импл живёт в graft-модуле (proxy-DLL рядом с exe); без неё скрипт не слинкуется.
// Модуль: 3_Game

#ifdef GRAFTED_DP_DBStorage
proto native owned string DbVersion();
proto native owned string DbStatus();
proto native int DbConfigValue(string key);
proto native owned string DbPlayerBegin(string uid, string name, int alive, float x, float y, float z, float yaw, float health, float blood, float water, float energy);
proto native int DbPlayerItem(int parent, string slot, string type, float health, float quantity, int ammo, int liquid_type, float liquid_vol, int food_stage, float wetness, float temperature, string extra);
proto native owned string DbPlayerEnd();
proto native owned string DbWorldBegin();
proto native owned string DbWorldMark(string layer);
proto native int DbWorldEntity(string layer, string type, float x, float y, float z, float yaw, float pitch, float roll, float health);
proto native int DbWorldItem(int handle, int parent, string slot, string type, float health, float quantity, int ammo, int liquid_type, float liquid_vol, int food_stage, float wetness, float temperature, string extra);
proto native owned string DbWorldEnd();
#endif
