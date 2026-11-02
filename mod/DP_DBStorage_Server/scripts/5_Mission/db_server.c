// DP_DBStorage — мост Enforce↔нативы плагина (5_Mission).
// Фаза 0: smoke-test нативов при старте миссии. Сохранение/восстановление
// (DbPlayerSaver/DbWorldSaver/DbRestorer) — следующие фазы.
#ifdef GRAFTED_DP_DBStorage

modded class MissionServer
{
	override void OnInit()
	{
		super.OnInit();
		Print("[DP_DBStorage] " + DbVersion());
	}
}

#endif
