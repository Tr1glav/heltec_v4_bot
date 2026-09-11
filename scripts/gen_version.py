import hashlib
import os
from SCons.Script import COMMAND_LINE_TARGETS

Import("env")

# Версия MAJOR.MINOR.BUILD: BUILD растёт, только когда меняются исходники прошивки.
# version.txt: строка 1 — версия (MAJOR/MINOR правятся руками), строка 2 — хэш исходников этой версии.
PROJECT_DIR = env.subst("$PROJECT_DIR")
VERSION_FILE = os.path.join(PROJECT_DIR, "version.txt")
SOURCE_PATHS = ["src", "lib", "include", "boards", "variants", "platformio.ini"]

# Служебные запуски (IntelliSense в IDE, clean) тоже исполняют pre-скрипты — версию не трогаем
NO_BUMP_TARGETS = {"idedata", "__idedata", "clean", "cleanall", "envdump", "compiledb", "menuconfig", "size"}


def source_files():
    for path in SOURCE_PATHS:
        full = os.path.join(PROJECT_DIR, path)
        if os.path.isfile(full):
            yield path
        for root, dirs, files in os.walk(full):
            dirs[:] = [d for d in dirs if not d.startswith(".") and d != "__pycache__"]
            for name in files:
                if not name.startswith("."):
                    yield os.path.relpath(os.path.join(root, name), PROJECT_DIR)


def sources_hash():
    h = hashlib.sha1()
    for rel in sorted(source_files()):
        h.update(rel.replace(os.sep, "/").encode())
        with open(os.path.join(PROJECT_DIR, rel), "rb") as fh:
            h.update(fh.read())
    return h.hexdigest()[:12]


with open(VERSION_FILE) as fh:
    fields = fh.read().split()
saved_hash = fields[1] if len(fields) > 1 else None
major, minor, build = (int(p) for p in fields[0].split("."))
current_hash = sources_hash()

if saved_hash != current_hash and not NO_BUMP_TARGETS & set(COMMAND_LINE_TARGETS):
    # без сохранённого хэша текущая версия просто привязывается к текущему коду
    if saved_hash is not None:
        build += 1
    with open(VERSION_FILE, "w") as fh:
        fh.write(f"{major}.{minor}.{build}\n{current_hash}\n")

version = f"{major}.{minor}.{build}"
print(f"[gen_version] FW_VERSION = {version} (src {current_hash})")
env.Append(CPPDEFINES=[("FW_VERSION", env.StringifyMacro(version))])
