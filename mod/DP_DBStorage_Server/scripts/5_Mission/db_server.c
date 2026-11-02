// DP_DBStorage — серверная часть мода: сохранение в MariaDB (фаза 1).
//
// Правило совместимости: нативы существуют только под ядром GRAFT (объявления
// генерируются под #ifdef GRAFTED_DP_DBStorage), поэтому весь код — под тем же
// дефайном. Без hid.dll мод компилируется пустым.
//
// Правило Enforce (грабли DP-продакшена): условия и конкатенации — ОДНОЙ
// строкой; foreach через ДВОЕТОЧИЕ.
//
// Сохранение игрока — InvokeOnConnect/Disconnect + периодика; мира — один
// решёточный проход сразу по всем слоям (разведка DP: радиус 300 / шаг 400,
// клетка всегда считается обработанной, дедуп по GetID — клетки перекрываются).

#ifdef GRAFTED_DP_DBStorage

class DbApi
{
	static void Log(string s)
	{
		Print("[DP_DBStorage] " + s);
	}

	// Слот предмета в компактном виде для восстановления (фаза 2):
	//   S<slotId> — аттач, C<row>_<col> — карго, H — руки, "" — корень.
	static string SlotOf(EntityAI item)
	{
		if (!item.GetInventory())
			return "";
		InventoryLocation loc = new InventoryLocation();
		if (!item.GetInventory().GetCurrentInventoryLocation(loc))
			return "";
		int t = loc.GetType();
		if (t == InventoryLocationType.ATTACHMENT)
			return "S" + loc.GetSlot().ToString();
		if (t == InventoryLocationType.CARGO || t == InventoryLocationType.PROXYCARGO)
			return "C" + loc.GetRow().ToString() + "_" + loc.GetCol().ToString();
		if (t == InventoryLocationType.HANDS)
			return "H";
		return "";
	}

	// Один предмет в staging. forPlayer: DbPlayerItem, иначе DbWorldItem(handle).
	// Возвращает 1-основанный индекс (0 = отклонён — см. DbStatus).
	static int PushItem(bool forPlayer, int handle, int parent, EntityAI item)
	{
		float health = item.GetHealth("", "");
		float quantity = -1;
		int ammo = -1;
		int liquid = -1;
		float liquidVol = -1;
		int foodStage = -1;
		float wet = 0;
		float temp = 0;
		ItemBase ib = ItemBase.Cast(item);
		if (ib)
		{
			quantity = ib.GetQuantity();
			liquid = ib.GetLiquidType();
			liquidVol = quantity;
			wet = ib.GetWet();
			temp = ib.GetTemperature();
		}
		Magazine mg = Magazine.Cast(item);
		if (mg)
			ammo = mg.GetAmmoCount();
		Edible_Base ed = Edible_Base.Cast(item);
		if (ed && ed.HasFoodStage())
			foodStage = ed.GetFoodStageType();
		if (liquid == 0)
		{
			liquid = -1;
			liquidVol = -1;
		}
		string slot = SlotOf(item);
		if (forPlayer)
			return DbPlayerItem(parent, slot, item.GetType(), health, quantity, ammo, liquid, liquidVol, foodStage, wet, temp, "");
		return DbWorldItem(handle, parent, slot, item.GetType(), health, quantity, ammo, liquid, liquidVol, foodStage, wet, temp, "");
	}

	// Обход инвентаря: parent-связи по фактической иерархии (PREORDER — родитель
	// всегда раньше ребёнка, но индексы берём из карты, а не из порядка).
	static int DumpInventory(bool forPlayer, int handle, EntityAI root)
	{
		if (!root.GetInventory())
			return 0;
		array<EntityAI> items = new array<EntityAI>;
		root.GetInventory().EnumerateInventory(InventoryTraversalType.PREORDER, items);
		// ключи Enforce-таблиц — только value-типы: вместо ссылок GetID()
		map<int, int> idx = new map<int, int>;
		int pushed = 0;
		foreach (EntityAI e : items)
		{
			if (e == root)
				continue;
			int parentIdx = 0;
			EntityAI p = e.GetHierarchyParent();
			if (p && idx.Contains(p.GetID()))
				parentIdx = idx.Get(p.GetID());
			int my = PushItem(forPlayer, handle, parentIdx, e);
			if (my > 0)
			{
				idx.Set(e.GetID(), my);
				pushed++;
			}
		}
		return pushed;
	}

	// Полный снапшот игрока: скаляры + дерево инвентаря.
	static void SavePlayer(PlayerBase player)
	{
		PlayerIdentity identity = player.GetIdentity();
		if (!identity)
			return;
		vector pos = player.GetPosition();
		vector ori = player.GetOrientation();
		float water = -1;
		if (player.GetStatWater())
			water = player.GetStatWater().Get();
		float energy = -1;
		if (player.GetStatEnergy())
			energy = player.GetStatEnergy().Get();
		string rc = DbPlayerBegin(identity.GetId(), identity.GetName(), player.IsAlive(), pos[0], pos[1], pos[2], ori[0], player.GetHealth("", "Health"), player.GetHealth("", "Blood"), water, energy);
		if (rc != "")
		{
			Log("save player " + identity.GetId() + " fail: " + rc);
			return;
		}
		int items = DumpInventory(true, 0, player);
		rc = DbPlayerEnd();
		if (rc != "")
			Log("save player end fail: " + rc);
		else
			Log("save player " + identity.GetId() + " items=" + items.ToString());
	}

	// Слой сущности мира — классификация DP_Storage (проверена на проде):
	// игроки (IsMan) не ходят никогда; лут — только корневые ItemBase.
	static string LayerOf(Object obj)
	{
		if (obj.IsInherited(Car))
			return "vehicles";
		if (obj.IsInherited(ZombieBase))
			return "zombies";
		if (obj.IsInherited(AnimalBase))
			return "animals";
		if (obj.IsInherited(BaseBuildingBase))
			return "buildings";
		if (obj.IsInherited(CrashBase) || obj.IsInherited(EffectArea))
			return "events";
		ItemBase item = ItemBase.Cast(obj);
		if (item && item.GetHierarchyRoot() == item)
		{
			if (item.IsInherited(UndergroundStash) || item.GetType() == "WoodenCrate")
				return "stashes";
			return "dynamic";
		}
		return "";
	}
};

// Решёточный проход по карте: одна сессия собирает ВСЕ слои сразу. Клетка
// обрабатывается целиком (плотная клетка без обрезки — иначе потеряем сущности);
// темп держим числом клеток на тик.
class DbWorldScan
{
	float m_CellRadius = 300;
	float m_CellStep = 400;
	int m_CellsPerTick = 8;

	ref map<int, int> m_Seen;
	int m_Idx = 0;
	int m_Grid = 0;
	int m_TotalCells = 0;
	int m_Entities = 0;
	int m_Items = 0;
	bool m_Done = false;

	void DbWorldScan()
	{
		m_Seen = new map<int, int>;
		DbWorldBegin();
		DbWorldMark("dynamic");
		DbWorldMark("vehicles");
		DbWorldMark("zombies");
		DbWorldMark("animals");
		DbWorldMark("buildings");
		DbWorldMark("stashes");
		DbWorldMark("events");
		float size = GetGame().GetWorld().GetWorldSize();
		m_Grid = Math.Ceil(size / m_CellStep);
		m_TotalCells = m_Grid * m_Grid;
		DbApi.Log("world scan begin: cells=" + m_TotalCells.ToString());
		GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(this.Work, 300, true);
	}

	bool IsDone()
	{
		return m_Done;
	}

	void Work()
	{
		if (m_Done)
			return;

		int processed = 0;
		while (processed < m_CellsPerTick && m_Idx < m_TotalCells)
		{
			int ix = m_Idx % m_Grid;
			int iz = m_Idx / m_Grid;

			array<Object> objects = new array<Object>;
			GetGame().GetObjectsAtPosition(Vector((ix + 0.5) * m_CellStep, 0, (iz + 0.5) * m_CellStep), m_CellRadius, objects, null);

			for (int i = 0; i < objects.Count(); i++)
			{
				Object obj = objects.Get(i);
				if (obj.IsMan())
					continue;
				int objId = obj.GetID();
				if (m_Seen.Contains(objId))
					continue;
				m_Seen.Set(objId, 1);
				string layer = DbApi.LayerOf(obj);
				if (layer == "")
					continue;
				EntityAI ent = EntityAI.Cast(obj);
				if (!ent)
					continue;
				vector pos = obj.GetPosition();
				vector ori = obj.GetOrientation();
				int handle = DbWorldEntity(layer, obj.GetType(), pos[0], pos[1], pos[2], ori[0], ori[1], ori[2], ent.GetHealth("", ""));
				if (handle <= 0)
					continue;
				m_Entities++;
				m_Items += DbApi.DumpInventory(false, handle, ent);
			}

			processed++;
			// клетка всегда считается обработанной (урок DP: зависание на плотной клетке)
			m_Idx++;
		}

		if (m_Idx >= m_TotalCells)
		{
			m_Done = true;
			string rc = DbWorldEnd();
			DbApi.Log("world scan done: entities=" + m_Entities.ToString() + " items=" + m_Items.ToString() + " end=" + rc);
			DbApi.Log("status: " + DbStatus());
		}
	}
};

class DbManager
{
	static ref DbManager s_Instance;

	ref DbWorldScan m_Scan;

	static DbManager Get()
	{
		if (!s_Instance)
			s_Instance = new DbManager();
		return s_Instance;
	}

	void Init()
	{
		int playerSec = DbConfigValue("player_save_sec");
		if (playerSec < 10)
			playerSec = 60;
		int worldSec = DbConfigValue("world_save_sec");
		if (worldSec < 30)
			worldSec = 300;
		GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(this.SavePlayers, playerSec * 1000, true);
		GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(this.WorldCycle, worldSec * 1000, true);
		DbApi.Log("manager: player every " + playerSec.ToString() + "s, world every " + worldSec.ToString() + "s; " + DbVersion());
	}

	void SavePlayers()
	{
		array<Man> players = new array<Man>;
		GetGame().GetPlayers(players);
		int saved = 0;
		for (int i = 0; i < players.Count(); i++)
		{
			PlayerBase pb = PlayerBase.Cast(players.Get(i));
			if (pb)
			{
				DbApi.SavePlayer(pb);
				saved++;
			}
		}
		if (saved > 0)
			DbApi.Log("periodic players saved: " + saved.ToString());
	}

	void WorldCycle()
	{
		if (!m_Scan || m_Scan.IsDone())
			m_Scan = new DbWorldScan();
	}
};

modded class MissionServer
{
	override void OnInit()
	{
		super.OnInit();
		DbManager.Get().Init();
	}

	override void InvokeOnConnect(PlayerBase player, PlayerIdentity identity)
	{
		super.InvokeOnConnect(player, identity);
		if (player && identity)
			DbApi.SavePlayer(player);
	}

	override void InvokeOnDisconnect(PlayerBase player)
	{
		if (player)
			DbApi.SavePlayer(player);
		super.InvokeOnDisconnect(player);
	}
}

#endif
