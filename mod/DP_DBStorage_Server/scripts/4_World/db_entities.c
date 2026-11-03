// DP_DBStorage — события мира в MariaDB (таблица world_events).
// МОДУЛЬ 4_World: entity-классы (ItemBase/PlayerBase/ИИ) модятся только из
// родного модуля — поэтому файл здесь, а не в 5_Mission.
//
// ПРАВИЛО ШУМА: каждый коллектор — одна строка условия + один натив;
// DbEvent при events=0 отвечает 0 не сходя с места. Позиции — дельта >3 м
// раз в секунду (шаг человека ~1.5 м/с), иначе журнал забьётся топтанием.
// Всплеск item_spawn при буте (CE-раскладка ~20k) — норма: буфер плагина
// держит 65536, выгрузка пачками по 500 каждую секунду.
//
// События: item_spawn (EOnInit), item_move (OnItemLocationChanged),
// item_destroyed (EEKilled у предметов), player_death, ai_death
// (зомби/животные), player_pos (таймер менеджера), player_connect/
// player_disconnect (db_server.c).
#ifdef GRAFTED_DP_DBStorage

class DbEvents
{
	static int s_On = -1;   // кэш DbConfigValue("events"); -1 = ещё не читали

	static bool On()
	{
		if (s_On < 0)
			s_On = DbConfigValue("events");
		return s_On == 1;
	}

	static void At(EntityAI e, out float x, out float y, out float z)
	{
		vector p = e.GetPosition();
		x = p[0];
		y = p[1];
		z = p[2];
	}

	static void Spawn(EntityAI e, string type)
	{
		if (!On() || !e)
			return;
		float x, y, z;
		At(e, x, y, z);
		DbEvent("item_spawn", "", type, x, y, z, "");
	}

	static void Move(EntityAI e, string type, EntityAI old_owner, EntityAI new_owner)
	{
		if (!On() || !e)
			return;
		string payload = "old=";
		if (old_owner)
			payload += old_owner.GetType();
		payload += " new=";
		if (new_owner)
			payload += new_owner.GetType();
		float x, y, z;
		At(e, x, y, z);
		DbEvent("item_move", "", type, x, y, z, payload);
	}

	static void Death(Object o, string type, string kind)
	{
		if (!On() || !o)
			return;
		vector p = o.GetPosition();
		DbEvent(kind, "", type, p[0], p[1], p[2], "");
	}

	static void PlayerPos(PlayerBase p, string uid)
	{
		if (!On() || !p || uid == "")
			return;
		vector pos = p.GetPosition();
		float dx = pos[0] - p.m_DbLastPos[0];
		float dz = pos[2] - p.m_DbLastPos[2];
		if (dx * dx + dz * dz < 9.0)
			return;   // < 3 м — не событие
		p.m_DbLastPos = pos;
		DbEvent("player_pos", uid, "", pos[0], pos[1], pos[2], "");
	}
};

modded class ItemBase
{
	override void EOnInit(IEntity other, int extra)
	{
		super.EOnInit(other, extra);
		DbEvents.Spawn(this, GetType());
	}

	override void OnItemLocationChanged(EntityAI old_owner, EntityAI new_owner)
	{
		super.OnItemLocationChanged(old_owner, new_owner);
		DbEvents.Move(this, GetType(), old_owner, new_owner);
	}

	override void EEKilled(Object killer)
	{
		DbEvents.Death(this, GetType(), "item_destroyed");
		super.EEKilled(killer);
	}
}

modded class PlayerBase
{
	vector m_DbLastPos = "0 0 0";

	override void EEKilled(Object killer)
	{
		string uid = "";
		if (GetIdentity())
			uid = GetIdentity().GetId();
		vector p = GetPosition();
		if (uid != "")
			DbEvent("player_death", uid, "", p[0], p[1], p[2], "");
		super.EEKilled(killer);
	}
}

modded class ZombieBase
{
	override void EEKilled(Object killer)
	{
		DbEvents.Death(this, GetType(), "ai_death");
		super.EEKilled(killer);
	}
}

modded class AnimalBase
{
	override void EEKilled(Object killer)
	{
		DbEvents.Death(this, GetType(), "ai_death");
		super.EEKilled(killer);
	}
}

#endif
