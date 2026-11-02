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

// ── восстановление ───────────────────────────────────────────────────────────
// Форматы packed-строк (TAB, Split("\t")) — restore.cpp: сущность
// type/x/y/z/yaw/pitch/roll/health; предмет parent/slot/type/health/quantity/
// ammo/liquid_type/liquid_vol/food_stage/wetness/temperature/extra; слот:
// S<slotId> аттач, C<row>_<col> карго, H руки, "" корень.

class DbRestorer
{
	static ref DbRestorer s_Active;

	static const int PER_TICK = 100;   // сущностей за тик: спавн дороже дампа

	ref array<string> m_Layers;
	int m_LayerI = 0;
	int m_EntI = 0;
	int m_Spawned = 0;
	int m_Items = 0;
	int m_Failed = 0;
	bool m_Done = false;

	void DbRestorer()
	{
		m_Layers = new array<string>;
		m_Layers.Insert("dynamic");
		m_Layers.Insert("vehicles");
		m_Layers.Insert("zombies");
		m_Layers.Insert("animals");
		m_Layers.Insert("buildings");
		m_Layers.Insert("stashes");
		m_Layers.Insert("events");
		s_Active = this;
		DbApi.Log("restore begin: слоёв=" + m_Layers.Count().ToString());
		GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(this.Work, 500, true);
	}

	static bool IsRunning()
	{
		return s_Active && !s_Active.m_Done;
	}

	// Состояние предмета (quantity/патроны/жидкость/мокрость/температура).
	static void ApplyItemState(EntityAI item, array<string> f)
	{
		ItemBase ib = ItemBase.Cast(item);
		if (ib)
		{
			if (f.Get(4).ToFloat() >= 0)
				ib.SetQuantity(f.Get(4).ToFloat());
			if (f.Get(6).ToInt() > 0 && f.Get(7).ToFloat() >= 0)
			{
				ib.SetLiquidType(f.Get(6).ToInt());
				ib.SetQuantity(f.Get(7).ToFloat());
			}
			if (f.Get(9).ToFloat() >= 0)
				ib.SetWet(f.Get(9).ToFloat());
		}
		Magazine mg = Magazine.Cast(item);
		if (mg && f.Get(5).ToInt() >= 0)
			mg.ServerSetAmmoCount(f.Get(5).ToInt());
		item.SetHealth("", "", f.Get(3).ToFloat());
	}

	// Размещение предмета в дереве: byIdx — уже восстановленные предметы узла.
	// Точный слот — InventoryLocation + LocationSyncMoveEntity (API 1.30;
	// TakeEntityToTargetInventory ищет слот сам, нам нужен конкретный).
	// Возвращает placed EntityAI или null.
	static EntityAI PlaceItem(string packed, EntityAI root, PlayerBase forPlayer, map<int, EntityAI> byIdx, vector rootPos)
	{
		array<string> f = new array<string>;
		packed.Split("\t", f);
		if (f.Count() < 11)
			return null;
		EntityAI item = EntityAI.Cast(GetGame().CreateObjectEx(f.Get(2), rootPos, ECE_LOCAL));
		if (!item)
			return null;
		ApplyItemState(item, f);
		EntityAI parentEnt = root;
		if (f.Get(0).ToInt() > 0 && byIdx.Contains(f.Get(0).ToInt()))
			parentEnt = byIdx.Get(f.Get(0).ToInt());
		string slot = f.Get(1);
		if (slot == "-")
			slot = "";   // пустые поля пакуются дефисом (Split роняет пустышки)
		if (slot == "" || !parentEnt)
			return item;   // корень — остаётся как создан
		// откуда двигать: свежесозданный предмет лежит в мире
		InventoryLocation src = new InventoryLocation();
		if (!item.GetInventory().GetCurrentInventoryLocation(src))
			return item;
		InventoryLocation dst = new InventoryLocation();
		bool made = false;
		if (slot.Length() > 0 && slot.Substring(0, 1) == "S")
		{
			dst.SetAttachment(parentEnt, item, slot.Substring(1, slot.Length() - 1).ToInt());
			made = true;
		}
		else if (slot == "H" && forPlayer)
		{
			dst.SetHands(forPlayer, item);
			made = true;
		}
		else if (slot.Length() > 1 && slot.Substring(0, 1) == "C")
		{
			array<string> rc = new array<string>;
			slot.Substring(1, slot.Length() - 1).Split("_", rc);
			if (rc.Count() >= 2)
			{
				dst.SetCargo(parentEnt, item, 0, rc.Get(0).ToInt(), rc.Get(1).ToInt(), false);
				made = true;
			}
		}
		if (made && GameInventory.LocationSyncMoveEntity(src, dst))
			return item;
		// слот не принял (занят/несовместим): предмет остаётся под корнем
		DbApi.Log("restore: слот не принял " + f.Get(2) + " slot=" + slot + " — оставлен на земле");
		return item;
	}

	void Work()
	{
		if (m_Done)
			return;
		if (m_LayerI >= m_Layers.Count())
		{
			m_Done = true;
			DbApi.Log("restore done: entities=" + m_Spawned.ToString() + " items=" + m_Items.ToString() + " failed=" + m_Failed.ToString());
			return;
		}
		string layer = m_Layers.Get(m_LayerI);
		int total = DbRestoreCount(layer);
		if (total < 0)
			return;   // слой ещё греется в кэше воркера — ждём этот же тик-таймер

		int n = 0;
		while (n < PER_TICK && m_EntI < total)
		{
			string packed = DbRestoreEntity(layer, m_EntI);
			if (packed != "")
			{
				array<string> f = new array<string>;
				packed.Split("\t", f);
				if (f.Count() >= 8)
				{
					EntityAI ent = EntityAI.Cast(GetGame().CreateObjectEx(f.Get(0), Vector(f.Get(1).ToFloat(), f.Get(2).ToFloat(), f.Get(3).ToFloat()), ECE_PLACE_ON_SURFACE | ECE_TRACE));
					if (ent)
					{
						ent.SetOrientation(Vector(f.Get(4).ToFloat(), f.Get(5).ToFloat(), f.Get(6).ToFloat()));
						ent.SetHealth("", "", f.Get(7).ToFloat());
						m_Spawned++;
						int items = DbRestoreItemsCount(layer, m_EntI);
						if (items > 0)
						{
							map<int, EntityAI> byIdx = new map<int, EntityAI>;
							for (int i = 0; i < items; i++)
							{
								EntityAI placed = PlaceItem(DbRestoreItem(layer, m_EntI, i), ent, null, byIdx, ent.GetPosition());
								if (placed)
								{
									byIdx.Set(i + 1, placed);
									m_Items++;
								}
								else
									m_Failed++;
							}
						}
					}
					else
						m_Failed++;
				}
			}
			m_EntI++;
			n++;
		}
		if (m_EntI >= total)
		{
			DbApi.Log("restore layer " + layer + ": " + total.ToString() + " сущностей");
			m_LayerI++;
			m_EntI = 0;
		}
	}
};

// Персонаж: запрос уходит воркеру, готовность — опросом раз в секунду.
class DbPlayerRestore
{
	string m_Uid;
	PlayerBase m_Player;
	int m_Ticks = 0;

	void DbPlayerRestore(PlayerBase player, string uid)
	{
		m_Player = player;
		m_Uid = uid;
		GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(this.Poll, 1000, true);
	}

	void Stop()
	{
		GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).Remove(this.Poll);
	}

	void Poll()
	{
		m_Ticks++;
		string packed = DbPlayerLoad(m_Uid);
		if (packed == "")
		{
			if (m_Ticks > 60)
			{
				DbApi.Log("restore player " + m_Uid + ": таймаут чтения из СУБД");
				Stop();
			}
			return;
		}
		Stop();
		if (packed == "MISS" || !m_Player)
			return;   // свежий персонаж — движок уже создал
		array<string> f = new array<string>;
		packed.Split("\t", f);
		if (f.Count() < 10 || f.Get(0).ToInt() == 0)
			return;   // мёртвый персонаж — как свежий (рестарт-флоу движка)
		if (!m_Player.IsAlive())
			return;
		m_Player.SetPosition(Vector(f.Get(1).ToFloat(), f.Get(2).ToFloat(), f.Get(3).ToFloat()));
		m_Player.SetOrientation(Vector(f.Get(4).ToFloat(), 0, 0));
		m_Player.SetHealth("", "Health", f.Get(5).ToFloat());
		m_Player.SetHealth("", "Blood", f.Get(6).ToFloat());
		if (m_Player.GetStatWater() && f.Get(7).ToFloat() >= 0)
			m_Player.GetStatWater().Set(f.Get(7).ToFloat());
		if (m_Player.GetStatEnergy() && f.Get(8).ToFloat() >= 0)
			m_Player.GetStatEnergy().Set(f.Get(8).ToFloat());
		int items = DbPlayerItemsCount(m_Uid);
		int restored = 0;
		if (items > 0)
		{
			map<int, EntityAI> byIdx = new map<int, EntityAI>;
			for (int i = 0; i < items; i++)
			{
				EntityAI placed = DbRestorer.PlaceItem(DbPlayerLoadItem(m_Uid, i), m_Player, m_Player, byIdx, m_Player.GetPosition());
				if (placed)
				{
					byIdx.Set(i + 1, placed);
					restored++;
				}
			}
		}
		string pname = f.Get(9);
		if (pname == "-")
			pname = "?";
		DbApi.Log("restore player " + m_Uid + " (" + pname + "): предметов=" + restored.ToString());
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
		// streams: persistence делает сам движок (IAT-перенаправление в СУБД);
		// рестореры не нужны, но построчные снимки включаем — читаемость в MySQL.
		string mode = DbModeText();
		bool streams = mode.Length() >= 7 && mode.Substring(0, 7) == "streams";
		int playerSec = DbConfigValue("player_save_sec");
		if (playerSec < 10)
			playerSec = 60;
		int worldSec = DbConfigValue("world_save_sec");
		if (worldSec < 30)
			worldSec = 300;
		GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(this.SavePlayers, playerSec * 1000, true);
		GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(this.WorldCycle, worldSec * 1000, true);
		if (streams)
			DbApi.Log("manager: streams — persistence нативный, снимки: игрок каждые " + playerSec.ToString() + "с, мир каждые " + worldSec.ToString() + "с; " + DbVersion());
		else
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
		if (DbRestorer.IsRunning())
			return;   // мир ещё восстанавливается — не затирать снапшот частичным
		if (!m_Scan || m_Scan.IsDone())
			m_Scan = new DbWorldScan();
	}
};

modded class MissionServer
{
	ref map<string, ref DbPlayerRestore> m_DbRestores;
	bool m_DbStreams = false;

	override void OnInit()
	{
		super.OnInit();
		m_DbRestores = new map<string, ref DbPlayerRestore>;
		string mode = DbModeText();
		if (mode.Length() >= 7 && mode.Substring(0, 7) == "streams")
			m_DbStreams = true;
		DbManager.Get().Init();
		if (!m_DbStreams)
			new DbRestorer();
	}

	override void InvokeOnConnect(PlayerBase player, PlayerIdentity identity)
	{
		super.InvokeOnConnect(player, identity);
		if (player && identity)
		{
			DbApi.SavePlayer(player);
			if (!m_DbStreams)
			{
				DbPlayerRequest(identity.GetId());
				m_DbRestores.Set(identity.GetId(), new DbPlayerRestore(player, identity.GetId()));
			}
		}
	}

	override void InvokeOnDisconnect(PlayerBase player)
	{
		if (player)
			DbApi.SavePlayer(player);
		super.InvokeOnDisconnect(player);
	}
}

#endif
