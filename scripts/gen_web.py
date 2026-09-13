import gzip
import io
import os

Import("env")

# Страница OTA хранится в web/ обычными файлами: их удобно править и проверять
# инструментами, а в прошивку они попадают уже сжатыми. Экономия примерно вчетверо,
# распаковку делает браузер по заголовку Content-Encoding.
PROJECT_DIR = env.subst("$PROJECT_DIR")
SRC = [("web/style.css", "WEB_STYLE_CSS_GZ"), ("web/app.js", "WEB_APP_JS_GZ")]
HEADER = os.path.join(PROJECT_DIR, "lib", "meshcore", "include", "web_assets.h")


def gz(data):
    # mtime=0: одинаковый вход даёт одинаковый выход, иначе версия прошивки скакала бы
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb", compresslevel=9, mtime=0) as f:
        f.write(data)
    return buf.getvalue()


out = ["// Сгенерировано scripts/gen_web.py из web/ — не править", "#pragma once", ""]
for path, name in SRC:
    raw = open(os.path.join(PROJECT_DIR, path), "rb").read()
    packed = gz(raw)
    rows = ",".join(f"0x{b:02x}" for b in packed)
    out.append(f"// {path}: {len(raw)} -> {len(packed)} байт")
    out.append(f"static const uint8_t {name}[] PROGMEM = {{{rows}}};")
    out.append(f"static const size_t {name}_LEN = {len(packed)};")
    out.append("")
    print(f"[gen_web] {path}: {len(raw)} -> {len(packed)} байт")

# Записываем только при изменении: лишняя перезапись заставила бы SCons пересобирать
# всё заново. Ранний выход здесь недопустим — SystemExit в pre-скрипте обрывает сборку.
text = "\n".join(out)
same = False
try:
    with open(HEADER) as fh:
        same = fh.read() == text
except OSError:
    same = False
if not same:
    with open(HEADER, "w") as fh:
        fh.write(text)
