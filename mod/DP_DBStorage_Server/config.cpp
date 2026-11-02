// DP_DBStorage_Server — PBO-часть graft-мода DP_DBStorage.
// Скрипты вызывают нативы плагина DP_DBStorage.grafted.dll; без ядра GRAFT весь
// код под #ifdef GRAFTED_DP_DBStorage и мод компилируется пустым (совместимость).

class CfgPatches
{
	class DP_DBStorage_Server
	{
		units[] = {};
		weapons[] = {};
		requiredVersion = 0.1;
		requiredAddons[] = { "DZ_Data" };
	};
};

class CfgMods
{
	class DP_DBStorage_Server
	{
		dir = "DP_DBStorage_Server";
		picture = "";
		action = "";
		hideName = 1;
		hidePicture = 1;
		name = "DP DBStorage (Server)";
		credits = "DarkProject";
		author = "DarkProject";
		authorID = "0";
		version = "0.1.0";
		extra = 0;
		type = "servermod";                // только сервер, клиентам не уходит

		dependencies[] = { "Core", "World", "Game", "Mission" };

		class defs
		{
			// сюда graft генерирует объявления нативов (3_Game).
			class gameScriptModule
			{
				value = "";
				files[] = { "DP_DBStorage_Server/scripts/3_Game" };
			};
			// вызовы: сохранение/восстановление, self-test миссии.
			class missionScriptModule
			{
				value = "";
				files[] = { "DP_DBStorage_Server/scripts/5_Mission" };
			};
		};
	};
};
