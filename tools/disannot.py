# Аннотированный дизассемблер для разведки: rip-ссылки на строки и импорты.
#   py -3.12 disannot.py <exe> <funcva> — печать функции с пояснениями.
import sys
import struct

sys.path.insert(0, r"C:\Grafted\RESEARCH\scripts")
import pe  # noqa: E402

img = pe.PE(sys.argv[1])
start = int(sys.argv[2], 0)
bound = img.func_at(start)
end = bound[1] if bound else start + 0x400

# IAT: адрес слота -> имя импорта
iat = {}
for slot, name in img.iat().items():
    iat.setdefault(slot, name)

# строки .rdata: va -> текст (лениво, по требованию)
def string_at(va):
    d = img.read(va, 200)
    if not d:
        return None
    z = d.find(b"\x00")
    s = d[: z if z >= 0 else 200]
    if len(s) < 4:
        return None
    try:
        t = s.decode("utf-8")
    except UnicodeDecodeError:
        return None
    if sum(1 for c in t if 32 <= ord(c) < 127 or ord(c) > 0x400) < len(t) * 0.9:
        return None
    return t

for line in img.dis(start, end - start).splitlines():
    parts = line.split(None, 2)
    if len(parts) < 3:   # «ret» без операндов
        print(line)
        continue
    addr_txt, mn, ops = parts
    va = int(addr_txt, 16)
    note = ""
    # rip-относительный операнд?
    if "rip" in ops:
        # вычислить цель: disp находится в байтах инструкции; перебором длин
        for ln in (4, 5, 6, 7, 8):
            off = img.off(va)
            if off is None or off + ln > len(img.d):
                break
            disp = struct.unpack_from("<i", img.d, off + ln - 4)[0]
            target = va + ln + disp
            t = string_at(target)
            if t:
                note = '  ; "%s"' % t[:80]
                break
            if target in iat:
                note = "  ; IAT %r" % (iat[target],)
                break
    print("%-8s %-8s %-46s%s" % (addr_txt, mn, ops, note))
