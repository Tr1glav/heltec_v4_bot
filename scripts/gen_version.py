import hashlib
import os
import subprocess
import time
from SCons.Script import COMMAND_LINE_TARGETS

Import("env")

# Версия MAJOR.MINOR.BUILD и время сборки. BUILD растёт, только когда меняются исходники прошивки.
# version.txt: строка 1 — версия (MAJOR/MINOR правятся руками), 2 — хэш исходников, 3 — unix-время версии.
# В код значения попадают через сгенерированный build_info.h, а не через -D: изменение глобального
# флага меняет команду компиляции каждого файла и заставляет пересобирать ядро Arduino и все библиотеки.
PROJECT_DIR = env.subst("$PROJECT_DIR")
VERSION_FILE = os.path.join(PROJECT_DIR, "version.txt")
HEADER = os.path.join(PROJECT_DIR, "lib", "meshcore", "include", "build_info.h")
SOURCE_PATHS = ["src", "lib", "web", "include", "boards", "variants", "platformio.ini"]

# Служебные запуски (IntelliSense в IDE, clean) тоже исполняют pre-скрипты — версию не трогаем
NO_BUMP_TARGETS = {"idedata", "__idedata", "clean", "cleanall", "envdump", "compiledb", "menuconfig", "size"}


def source_files():
    header_rel = os.path.relpath(HEADER, PROJECT_DIR)
    for path in SOURCE_PATHS:
        full = os.path.join(PROJECT_DIR, path)
        if os.path.isfile(full):
            yield path
        for root, dirs, files in os.walk(full):
            dirs[:] = [d for d in dirs if not d.startswith(".") and d != "__pycache__"]
            for name in files:
                rel = os.path.relpath(os.path.join(root, name), PROJECT_DIR)
                if not name.startswith(".") and rel != header_rel:
                    yield rel


def sources_hash():
    h = hashlib.sha1()
    for rel in sorted(source_files()):
        h.update(rel.replace(os.sep, "/").encode())
        with open(os.path.join(PROJECT_DIR, rel), "rb") as fh:
            h.update(fh.read())
    return h.hexdigest()[:12]


def write_if_changed(path, content):
    # не трогаем файл без нужды: для SCons перезапись того же содержимого — лишняя работа
    try:
        with open(path) as fh:
            if fh.read() == content:
                return
    except OSError:
        pass
    with open(path, "w") as fh:
        fh.write(content)


with open(VERSION_FILE) as fh:
    fields = fh.read().split()
major, minor, build = (int(p) for p in fields[0].split("."))
saved_hash = fields[1] if len(fields) > 1 else None
build_time = int(fields[2]) if len(fields) > 2 else int(time.time())
current_hash = sources_hash()

if not NO_BUMP_TARGETS & set(COMMAND_LINE_TARGETS):
    # без сохранённого хэша текущая версия просто привязывается к текущему коду
    if saved_hash is not None and saved_hash != current_hash:
        build += 1
        build_time = int(time.time())
    write_if_changed(VERSION_FILE, f"{major}.{minor}.{build}\n{current_hash}\n{build_time}\n")

def current_branch():
    """Имя ветки: в Actions оно есть в окружении, локально спрашиваем git."""
    name = os.environ.get("GITHUB_REF_NAME")
    if name:
        return name
    try:
        r = subprocess.run(["git", "rev-parse", "--abbrev-ref", "HEAD"],
                           cwd=PROJECT_DIR, capture_output=True, text=True)
        return r.stdout.strip()
    except OSError:
        return ""


# Чистый номер версии получают релизные сборки: в CI это сборка с ветки release/*, а
# локально — запуск с RELEASE=1, то есть тот, который и станет релизом. Всё остальное
# помечается как dev, чтобы на экране устройства сразу было видно, что прошивка не из
# релиза. В version.txt номер остаётся без суффикса.
#
# Оговорка: локально ветка release/* появляется только ПОСЛЕ сборки (её создаёт
# scripts/release.py пост-действием), поэтому ориентироваться на неё локально нельзя —
# отсюда и проверка переменной окружения.
version = f"{major}.{minor}.{build}"
is_release = os.environ.get("RELEASE") == "1" or current_branch().startswith("release/")
if not is_release:
    version += "dev"

write_if_changed(HEADER,
                 "// Сгенерировано scripts/gen_version.py перед сборкой — не править\n"
                 "#pragma once\n"
                 f"#define FW_VERSION \"{version}\"\n"
                 f"#define BUILD_UNIX_TIME {build_time}UL\n")
print(f"[gen_version] FW_VERSION = {version} (src {current_hash})")
