# Панели визуализации DP_DBStorage: карта мира по слоям + панель данных.
#
#   py -3.12 tools\panels.py                     # обе панели, открыть браузер
#   py -3.12 tools\panels.py --cfg <путь к dp_dbstorage.cfg>
#   py -3.12 tools\panels.py --no-open           # только записать HTML
#   py -3.12 tools\panels.py --host 10.0.0.5 --port 3306 --user dpstorage \
#                          --password секрет --db dp_storage
#
# ЗАЧЕМ. Статичные скриншоты в README устаревают; HeidiSQL требует установки.
# Генератор снимает текущее состояние MariaDB и печатает два самодостаточных
# HTML (без внешних библиотек и сети — можно отправить файл кому угодно):
#
#   dashboard.html — карта мира по слоям (интерактив: кнопки слоёв),
#                    метрики, топ типов, стримы persistence;
#   dbpanel.html   — панель данных: дерево объектов, сводка v_world_summary,
#                    последние db_commands с результатами, топ предметов.
#
# КАК. Только stdlib: запросы — mysql/mariadb-клиент из PATH (или --client),
# ответ разбирается из таб-разделённого -B. Данные панелей — те же таблицы,
# что пишет плагин (см. docs/visualization.md).
#
# Подключение берётся (по приоритету): явные аргументы → --cfg-файл → значения
# по умолчанию (локальный стенд).
import argparse
import json
import os
import random
import shutil
import subprocess
import sys
import time
from collections import Counter, defaultdict

DEFAULTS = dict(host="127.0.0.1", port=3311, user="dpstorage",
                password="dpdb_test", db="dp_storage")
WORLD = 15360.0          # размер карты в метрах (Чернорусь); оси: X — восток, Z — север
DYNAMIC_CAP = 2500       # прореживание слоя dynamic для карты (в БД — все строки)


def find_client(override):
    if override:
        return override
    for name in ("mariadb.exe", "mysql.exe"):
        p = shutil.which(name)
        if p:
            return p
    # стандартные места установки (MariaDB zip/MSI, стенд dp_dbstorage_test)
    import glob
    for pat in (r"C:\Program Files\MariaDB*\bin\mariadb.exe",
                r"C:\Program Files\MySQL\MySQL Server*\bin\mysql.exe",
                r"C:\dp_dbstorage_test\mariadb\bin\mariadb.exe"):
        hits = glob.glob(pat)
        if hits:
            return hits[0]
    sys.exit("mysql-клиент не найден: поставьте mariadb-клиент в PATH или "
             "укажите --client <путь>")


def read_cfg(path):
    """dp_dbstorage.cfg (ключ = значение) → словарь подключения."""
    out = {}
    with open(path, encoding="utf-8-sig") as f:
        for line in f:
            if "#" in line:
                line = line.split("#", 1)[0]
            if "=" not in line:
                continue
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


class Db:
    def __init__(self, client, conn):
        self.client = client
        self.conn = conn

    def rows(self, sql):
        cmd = [self.client, "-B", "-h", self.conn["host"],
               "-P", str(self.conn["port"]), "-u", self.conn["user"],
               self.conn["db"], "-e", sql]
        env = dict(os.environ, MYSQL_PWD=self.conn["password"])   # не в argv
        r = subprocess.run(cmd, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", env=env)
        if r.returncode != 0:
            sys.exit("запрос не удался: %s\n%s" % (sql, r.stderr.strip()))
        lines = [l for l in r.stdout.splitlines() if l and not l.startswith("(")]
        if not lines:
            return []
        head = lines[0].split("\t")
        return [dict(zip(head, l.split("\t"))) for l in lines[1:]]


def fmt(n):
    return "{:,}".format(int(n)).replace(",", " ")


# ── данные ───────────────────────────────────────────────────────────────────

def fetch(db):
    d = {}
    d["summary"] = db.rows("SELECT layer, entities FROM v_world_summary"
                           " ORDER BY entities DESC")
    ents = db.rows("SELECT layer, type, x, z FROM world_entities")
    by_layer = defaultdict(list)
    for e in ents:
        by_layer[e["layer"]].append((float(e["x"]), float(e["z"]), e["type"]))
    rnd = random.Random(7)   # фиксированный seed: панель воспроизводима
    d["points"], d["counts"], d["top"] = {}, {}, {}
    for layer, arr in by_layer.items():
        d["counts"][layer] = len(arr)
        shown = arr if len(arr) <= DYNAMIC_CAP else rnd.sample(arr, DYNAMIC_CAP)
        d["points"][layer] = [[round(x, 1), round(z, 1)] for x, z, _ in shown]
        d["top"][layer] = Counter(t for _, _, t in arr).most_common(5)
    d["total"] = len(ents)
    one = db.rows("SELECT (SELECT COUNT(*) FROM entity_items) items,"
                  " (SELECT COUNT(*) FROM shadow_files) streams,"
                  " (SELECT COALESCE(SUM(bytes),0) FROM shadow_files) bytes,"
                  " (SELECT COUNT(*) FROM shadow_players) players")
    d["extra"] = one[0] if one else dict(items=0, streams=0, bytes=0, players=0)
    d["streams"] = db.rows("SELECT name, bytes FROM shadow_files"
                           " ORDER BY bytes DESC LIMIT 8")
    d["types"] = db.rows("SELECT type, COUNT(*) n FROM entity_items"
                         " GROUP BY type ORDER BY n DESC LIMIT 6")
    d["cmds"] = db.rows("SELECT id, cmd, status, LEFT(result,60) res"
                        " FROM db_commands ORDER BY id DESC LIMIT 4")
    return d


# ── dashboard.html ───────────────────────────────────────────────────────────

LAYERS = [  # ключ, подпись, цвет (CSS-переменная), радиус, прозрачность
    ("dynamic", "Лут (dynamic)", 1, 1.3, .45),
    ("vehicles", "Транспорт", 2, 4.0, 1.0),
    ("zombies", "Зомби", 3, 3.2, 1.0),
    ("animals", "Животные", 4, 3.2, 1.0),
    ("events", "События", 5, 4.5, 1.0),
]


def build_dashboard(d):
    groups, legend = [], []
    for key, label, series, r, op in LAYERS:
        n_all = d["counts"].get(key, 0)
        circles = "".join(
            '<circle cx="%.1f" cy="%.1f" r="%s" style="fill:var(--s%d);opacity:%s"/>'
            % (x / WORLD * 760 + 20, 760 - z / WORLD * 760 + 20, r, series, op)
            for x, z in d["points"].get(key, []))
        groups.append('<g data-layer="%s">%s</g>' % (key, circles))
        legend.append('<button type="button" class="lyr" data-layer="%s" '
                      'aria-pressed="true"><span class="sw" style="background:'
                      'var(--s%d)"></span>%s <span class="cnt">%s</span>'
                      '</button>' % (key, series, label, fmt(n_all)))
    grid = labels = ""
    for i in range(6):
        p = 20 + i * 152
        grid += ('<line x1="%d" y1="20" x2="%d" y2="780" class="gl"/>'
                 '<line x1="20" y1="%d" x2="780" y2="%d" class="gl"/>'
                 % (p, p, p, p))
        labels += ('<text x="%d" y="795" text-anchor="middle" class="ax">%d</text>'
                   '<text x="26" y="%d" text-anchor="start" class="ax">%d</text>'
                   % (p, int(i * WORLD / 5), p, int(WORLD - i * WORLD / 5)))
    dyn = d["counts"].get("dynamic", 0)
    note = ("dynamic: показано %s из %s (прореживание для карты; в БД все)"
            % (fmt(min(dyn, DYNAMIC_CAP)), fmt(dyn))) if dyn > DYNAMIC_CAP else ""
    tops = "".join(
        '<tr><td>%s</td><td><code>%s</code></td><td class="r">%s</td></tr>'
        % (label if i == 0 else "", t, fmt(n))
        for key, label, *_ in LAYERS
        for i, (t, n) in enumerate(d["top"].get(key, [])[:3]))
    streams = "".join('<tr><td><code>%s</code></td><td class="r">%s</td></tr>'
                      % (s["name"], fmt(s["bytes"])) for s in d["streams"])
    return '''<!doctype html><html><head><meta charset="utf-8"><title>DP_DBStorage — карта мира</title>
<style>
:root{--fg:#0f172a;--mut:#64748b;--brd:#e2e8f0;--card:#f8fafc;
 --s1:#2563eb;--s2:#9333ea;--s3:#dc2626;--s4:#16a34a;--s5:#ea580c}
body{margin:28px;font-family:"Segoe UI",system-ui,sans-serif;color:var(--fg);background:#fff}
.grid{display:grid;grid-template-columns:repeat(4,1fr);gap:14px;margin:10px 0 18px}
.card{background:var(--card);border:1px solid var(--brd);border-radius:10px;padding:14px 18px}
.v{font-size:26px;font-weight:500;margin:2px 0} .sm{font-size:12px;color:var(--mut)}
.ctl{display:flex;flex-wrap:wrap;gap:6px;margin:8px 0}
.lyr{background:none;border:0;padding:2px 10px 2px 0;font-size:14px;cursor:pointer;
 display:inline-flex;align-items:center;gap:6px;color:var(--fg)}
.lyr[aria-pressed="false"]{opacity:.4}
.sw{width:10px;height:10px;border-radius:50%;display:inline-block}
.cnt{color:var(--mut)}
svg{width:100%;height:auto}
.gl{stroke:var(--brd);stroke-width:1}
.ax{fill:var(--mut);font-size:12px}
table{border-collapse:collapse;width:100%}
th{text-align:left;font-weight:500;color:var(--mut);border-bottom:1px solid var(--brd);padding:5px 10px;font-size:12px}
td{padding:5px 10px;border-bottom:1px solid #f1f5f9;font-size:13px}
.r{text-align:right;font-variant-numeric:tabular-nums}
code{background:#f1f5f9;border-radius:4px;padding:1px 5px;font-size:12px}
h2{font-size:20px;margin:14px 0 6px} h3{font-size:16px;margin:16px 0 6px}
</style></head><body>
<div class="grid">
 <div class="card"><div class="sm">Сущности мира</div><div class="v">__TOTAL__</div><div class="sm">world_entities, __LAYERS__ слоёв</div></div>
 <div class="card"><div class="sm">Предметы в сущностях</div><div class="v">__ITEMS__</div><div class="sm">entity_items</div></div>
 <div class="card"><div class="sm">Стримы persistence</div><div class="v">__NFILES__</div><div class="sm">__MB__ МБ в MariaDB</div></div>
 <div class="card"><div class="sm">Игроки (тени)</div><div class="v">__PLAYERS__</div><div class="sm">shadow_players</div></div>
</div>
<h2>Карта мира по слоям</h2>
<div class="ctl">__LEGEND__</div>
<svg viewBox="0 0 800 800" role="img" aria-label="Карта сущностей по слоям; X — восток, Z — север">
__GRID__<rect x="20" y="20" width="760" height="760" fill="none" style="stroke:var(--brd)"/>__GROUPS____LABELS__
<text x="400" y="12" text-anchor="middle" class="ax">Z, м (север ↑)</text>
<text x="792" y="400" text-anchor="middle" transform="rotate(90 792 400)" class="ax">X, м (восток →)</text>
</svg>
<p class="sm">__NOTE__</p>
<h3>Топ типов по слоям</h3>
<table><thead><tr><th>Слой</th><th>Тип</th><th class="r">Шт.</th></tr></thead><tbody>__TOPS__</tbody></table>
<h3>Крупнейшие стримы (shadow_files)</h3>
<table><thead><tr><th>Файл</th><th class="r">Байт</th></tr></thead><tbody>__STREAMS__</tbody></table>
<script>
document.querySelectorAll('.lyr').forEach(function(b){
 b.addEventListener('click',function(){
  var on=b.getAttribute('aria-pressed')!=='false';
  b.setAttribute('aria-pressed',on?'false':'true');
  var g=document.querySelector('g[data-layer="'+b.dataset.layer+'"]');
  if(g)g.style.display=on?'none':'';
 });
});
</script></body></html>'''.replace("__TOTAL__", fmt(d["total"])) \
        .replace("__LAYERS__", str(len(d["counts"]) or 1)) \
        .replace("__ITEMS__", fmt(d["extra"]["items"])) \
        .replace("__NFILES__", fmt(d["extra"]["streams"])) \
        .replace("__MB__", "%.1f" % (int(d["extra"]["bytes"]) / 1048576)) \
        .replace("__PLAYERS__", fmt(d["extra"]["players"])) \
        .replace("__LEGEND__", "".join(legend)) \
        .replace("__GRID__", grid).replace("__GROUPS__", "".join(groups)) \
        .replace("__LABELS__", labels).replace("__NOTE__", note) \
        .replace("__TOPS__", tops).replace("__STREAMS__", streams)


# ── dbpanel.html ─────────────────────────────────────────────────────────────

TREE = [("v_world_summary", True), ("v_players", False), ("v_help", False),
        ("world_entities", False), ("entity_items", False), ("players", False),
        ("player_items", False), ("shadow_files", False),
        ("shadow_players", False), ("db_commands", False),
        ("snapshots", False), ("snapshots_files", False)]


def build_dbpanel(d):
    tree = "".join('<div class="tr%s">%s %s</div>'
                   % (" sel" if sel else "", "▫" if n.startswith("v_") else "▪", n)
                   for n, sel in TREE)

    def table(data, head, right=(), code_cols=()):
        th = "".join('<th%s>%s</th>' % (' class="r"' if i in right else "", h)
                     for i, h in enumerate(head))
        trs = ""
        for r in data:
            tds = ""
            for i, c in enumerate(r):
                cls = ' class="r"' if i in right else ""
                txt = "<code>%s</code>" % c if head[i] in code_cols else c
                tds += "<td%s>%s</td>" % (cls, txt)
            trs += "<tr>%s</tr>" % tds
        return "<table><thead><tr>%s</tr></thead><tbody>%s</tbody></table>" % (th, trs)

    summary = [[s["layer"], s["entities"]] for s in d["summary"]]
    cmds = [[c["id"], c["cmd"], c["status"], c["res"]] for c in d["cmds"]]
    types = [[t["type"], t["n"]] for t in d["types"]]
    return '''<!doctype html><html><head><meta charset="utf-8"><title>DP_DBStorage — панель данных</title>
<style>
body{margin:0;font-family:"Segoe UI",system-ui,sans-serif;background:#f8fafc;color:#0f172a;font-size:14px}
.wrap{padding:26px 28px}
.win{display:flex;border:1px solid #cbd5e1;border-radius:10px;overflow:hidden;background:#fff}
.side{width:250px;background:#f1f5f9;border-right:1px solid #e2e8f0;padding:12px 10px}
.side h4{margin:2px 0 8px;font-size:12px;color:#64748b;font-weight:500}
.tr{padding:3px 8px;border-radius:5px;font-size:13px;color:#334155}
.tr.sel{background:#2563eb;color:#fff}
.main{flex:1;padding:14px 18px}
.tabs{display:flex;gap:16px;border-bottom:1px solid #e2e8f0;margin-bottom:10px;font-size:13px;color:#64748b}
.tabs .on{color:#0f172a;border-bottom:2px solid #2563eb;font-weight:500}
.q{background:#0f172a;color:#e2e8f0;border-radius:8px;padding:10px 14px;font-family:Consolas,monospace;font-size:13px;margin:6px 0 12px}
.q .k{color:#7dd3fc}.q .s{color:#fbbf24}
table{border-collapse:collapse;width:100%;margin-bottom:4px}
th{text-align:left;font-weight:500;color:#64748b;border-bottom:1px solid #e2e8f0;padding:5px 10px;font-size:12px}
td{padding:5px 10px;border-bottom:1px solid #f1f5f9;font-size:13px}
th.r,td.r{text-align:right;font-variant-numeric:tabular-nums}
code{background:#f1f5f9;border-radius:4px;padding:1px 5px;font-size:12px}
h3{font-size:13px;color:#64748b;font-weight:500;margin:14px 0 6px}
</style></head><body><div class="wrap">
<div style="font-size:20px;font-weight:500;margin-bottom:4px">dp_storage — панель данных</div>
<div style="color:#64748b;font-size:13px;margin-bottom:14px">__CONN__ · снимок от __TS__</div>
<div class="win"><div class="side"><h4>dp_storage</h4>__TREE__</div><div class="main">
<div class="tabs"><div class="on">Запрос</div><div>Данные</div><div>Структура</div></div>
<div class="q"><span class="k">SELECT</span> * <span class="k">FROM</span> <span class="s">v_world_summary</span> <span class="k">ORDER BY</span> entities <span class="k">DESC</span>;</div>
__T_SUMMARY__
<h3>Управление из любого mysql-клиента — db_commands</h3>
<div class="q"><span class="k">INSERT INTO</span> <span class="s">db_commands</span>(cmd, params) <span class="k">VALUES</span>('snapshot','перед вайпом');</div>
__T_CMDS__
<h3>Топ типов предметов — entity_items</h3>
__T_TYPES__
</div></div></div></body></html>''' \
        .replace("__CONN__", "%s@%s:%s" % (d["extra"].get("user", "dpstorage"),
                                           d["extra"].get("host", "127.0.0.1"),
                                           d["extra"].get("port", 3311))) \
        .replace("__TS__", d["extra"].get("ts", "")) \
        .replace("__TREE__", tree) \
        .replace("__T_SUMMARY__", table(summary, ["layer", "entities"], (1,), ("layer",))) \
        .replace("__T_CMDS__", table(cmds, ["id", "cmd", "status", "res"], (0,), ("cmd", "res"))) \
        .replace("__T_TYPES__", table(types, ["type", "n"], (1,), ("type",)))


def main():
    ap = argparse.ArgumentParser(description="Панели DP_DBStorage (см. docs/visualization.md)")
    ap.add_argument("--cfg", help="путь к dp_dbstorage.cfg — взять подключение оттуда")
    ap.add_argument("--host"), ap.add_argument("--port", type=int)
    ap.add_argument("--user"), ap.add_argument("--password")
    ap.add_argument("--db"), ap.add_argument("--client", help="путь к mysql/mariadb-клиенту")
    ap.add_argument("--out", default="out/panels", help="куда писать HTML (по умолчанию out/panels)")
    ap.add_argument("--no-open", action="store_true", help="не открывать браузер")
    ap.add_argument("--serve", action="store_true",
                    help="живой режим: локальный веб-сервер, панели актуальны сами")
    ap.add_argument("--bind", default="127.0.0.1",
                    help="адрес живого режима (по умолчанию только localhost)")
    ap.add_argument("--http", dest="http_port", type=int, default=8788,
                    help="порт живого режима (по умолчанию 8788)")
    ap.add_argument("--refresh", type=int, default=5,
                    help="живой режим: период обновления страницы, секунд (по умолчанию 5)")
    a = ap.parse_args()

    conn = dict(DEFAULTS)
    if a.cfg:
        c = read_cfg(a.cfg)
        for k in ("host", "user", "password"):
            if c.get(k):
                conn[k] = c[k]
        if c.get("port"):
            conn["port"] = int(c["port"])
        if c.get("database"):
            conn["db"] = c["database"]
    for k in ("host", "port", "user", "password", "db"):
        v = getattr(a, k)
        if v:
            conn[k] = v

    db = Db(find_client(a.client), conn)

    if a.serve:
        serve(db, conn, bind=a.bind, port=a.http_port, refresh=a.refresh,
              no_open=a.no_open)
        return

    d = fetch(db)
    import datetime
    d["extra"].update(user=conn["user"], host=conn["host"], port=conn["port"],
                      ts=datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"))

    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    dash = os.path.join(out, "dashboard.html")
    panel = os.path.join(out, "dbpanel.html")
    with open(dash, "w", encoding="utf-8") as f:
        f.write(build_dashboard(d))
    with open(panel, "w", encoding="utf-8") as f:
        f.write(build_dbpanel(d))
    print("сущностей %s, слои: %s" % (fmt(d["total"]),
          ", ".join("%s=%s" % (k, v) for k, v in sorted(d["counts"].items()))))
    print(dash)
    print(panel)
    if not a.no_open:
        os.startfile(dash)   # noqa: Windows only — как и весь мод
        os.startfile(panel)


# ── живой режим (intime) ─────────────────────────────────────────────────────
# ЗАЧЕМ: разовые панели — снимок; администратору нужно окно, которое всегда
# показывает «сейчас». --serve поднимает localhost-сервер (stdlib http.server):
# каждая загрузка страницы строит панели заново из базы, страница сама
# обновляется каждые --refresh секунд. Свежее данных в базе быть не может:
# плагин пишет их циклами sync_period_sec/world_save_sec — период обновления
# тоньше этих циклов не имеет смысла.

def serve(db, conn, bind, port, refresh, no_open):
    import datetime
    import threading
    from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

    lock = threading.Lock()          # mysql-клиент — по одному запросу за раз
    cache = {}                       # путь → (время сборки, html)

    def snapshot():
        d = fetch(db)
        d["extra"].update(user=conn["user"], host=conn["host"],
                          port=conn["port"],
                          ts=datetime.datetime.now().strftime("%H:%M:%S"))
        return d

    def live_html(page):
        with lock:
            built, html = cache.get(page, (0, ""))
            fresh = max(1, min(refresh, 2))   # не чаще раза в 1-2 с
            if time.time() - built > fresh:
                d = snapshot()
                html = (build_dashboard(d) if page == "dashboard"
                        else build_dbpanel(d))
                stamp = ("живой режим: обновление каждые %d с · снимок от %s"
                         % (refresh, d["extra"]["ts"]))
                html = html.replace(
                    "<head>",
                    '<head><meta http-equiv="refresh" content="%d">' % refresh, 1)
                html = html.replace(
                    "<body>",
                    '<body><p class="sm" style="margin:0 0 6px">%s</p>' % stamp, 1)
                cache[page] = (time.time(), html)
        return html.encode("utf-8")

    class H(BaseHTTPRequestHandler):
        def log_message(self, fmt_, *args):
            pass   # каждый refresh не логировать

        def do_GET(self):
            if self.path in ("/", "/index.html"):
                self.send_response(302)
                self.send_header("Location", "/dashboard.html")
                self.end_headers()
                return
            if self.path == "/dashboard.html":
                body = live_html("dashboard")
                mime = "text/html; charset=utf-8"
            elif self.path == "/dbpanel.html":
                body = live_html("dbpanel")
                mime = "text/html; charset=utf-8"
            elif self.path == "/status.json":
                with lock:
                    d = snapshot()
                body = json.dumps({
                    "updated": d["extra"]["ts"],
                    "entities_total": d["total"],
                    "layers": d["counts"],
                    "items": int(d["extra"]["items"]),
                    "streams": int(d["extra"]["streams"]),
                    "streams_bytes": int(d["extra"]["bytes"]),
                    "players": int(d["extra"]["players"]),
                }, ensure_ascii=False).encode("utf-8")
                mime = "application/json; charset=utf-8"
            else:
                self.send_response(404)
                self.end_headers()
                return
            self.send_response(200)
            self.send_header("Content-Type", mime)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

    srv = ThreadingHTTPServer((bind, port), H)
    url = "http://%s:%d/" % (bind, port)
    print("живой режим: %s (обновление %d с; Ctrl+C — стоп)" % (url, refresh))
    if not no_open:
        import webbrowser
        threading.Timer(0.5, lambda: webbrowser.open(url)).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nостановлено")


if __name__ == "__main__":
    main()
