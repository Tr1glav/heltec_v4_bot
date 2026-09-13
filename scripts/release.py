#!/usr/bin/env python3
"""Отправка кода в git после сборки и пометка релизных версий.

Схема:
  * каждая успешная сборка коммитит изменения и отправляет текущую ветку (develop);
  * с флагом --release дополнительно создаётся ветка release/v<версия> и уходит на origin.

Сами файлы прошивки в репозиторий не кладутся. Их собирает и выкладывает GitHub Actions
(.github/workflows/build.yml): появление ветки release/* запускает проверки и сборку, и
только если всё прошло — создаётся релиз с .bin и .otaz. Так в релиз физически не может
попасть прошивка, которая не собралась или не прошла тесты.

Вручную:
    python3 scripts/release.py                 коммит + отправка текущей ветки
    python3 scripts/release.py --release       то же + ветка release/v<версия>
    python3 scripts/release.py --dry-run       показать, что попадёт в коммит
    python3 scripts/release.py --no-push       только локально
    python3 scripts/release.py -m "текст"      своё описание коммита

Из сборки вызывается автоматически (scripts/copy_firmware.py):
    pio run                     коммит и отправка
    RELEASE=1 pio run           плюс релизная ветка
    NOGIT=1 pio run             ничего не трогать в git
"""
import argparse
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Пути, которых в коммите быть не должно ни при каких обстоятельствах. .gitignore их и так
# исключает, но цена ошибки — пароли и ключи каналов в истории публичного репозитория,
# поэтому проверяем ещё раз перед фиксацией.
FORBIDDEN = ("secrets.ini", "secrets.json", "firmware_output/", "build_info.h")


def git(*args, check=True):
    r = subprocess.run(["git", *args], cwd=str(ROOT), check=False, text=True,
                       capture_output=True)
    if check and r.returncode != 0:
        sys.exit(f"git {' '.join(args)}: {(r.stderr or r.stdout).strip()}")
    return (r.stdout or "").strip()


def git_try(*args):
    """Сетевые операции не должны ронять сборку: нет сети — просто сообщаем."""
    r = subprocess.run(["git", *args], cwd=str(ROOT), check=False, text=True,
                       capture_output=True, env={**os.environ, "GIT_TERMINAL_PROMPT": "0"})
    return r.returncode == 0, ((r.stderr or r.stdout).strip().splitlines() or [""])[-1]


def version():
    with open(ROOT / "version.txt") as fh:
        return fh.read().split()[0]


def main():
    ap = argparse.ArgumentParser(description="коммит, отправка и релизная ветка")
    ap.add_argument("--release", action="store_true",
                    help="создать ветку release/v<версия> — она запускает выкладку в Actions")
    ap.add_argument("--no-push", action="store_true", help="не отправлять на origin")
    ap.add_argument("--dry-run", action="store_true", help="только показать план")
    ap.add_argument("-m", "--message", help="описание коммита")
    args = ap.parse_args()

    ver = version()
    branch = git("rev-parse", "--abbrev-ref", "HEAD")
    rel_branch = f"release/v{ver}"

    git("add", "-A")
    files = [l for l in git("diff", "--cached", "--name-only").splitlines() if l]

    bad = [f for f in files if any(f.startswith(p) or f.endswith(p) for p in FORBIDDEN)]
    if bad:
        git("reset", check=False)
        sys.exit("[release] в коммит попали закрытые файлы, остановлено: " + ", ".join(bad))

    if args.dry_run:
        print(f"[release] версия {ver}, ветка {branch}, файлов к коммиту: {len(files)}")
        for f in files[:20]:
            print("   ", f)
        if len(files) > 20:
            print(f"    ... и ещё {len(files) - 20}")
        if args.release:
            print(f"[release] была бы создана ветка {rel_branch}")
        git("reset", check=False)
        return

    if files:
        git("commit", "-m", args.message or f"v{ver}")
        print(f"[release] коммит {git('rev-parse', '--short', 'HEAD')} в {branch}: файлов {len(files)}")
    else:
        git("reset", check=False)
        print(f"[release] изменений нет, коммит не нужен (версия {ver})")

    if args.release:
        git("branch", "-f", rel_branch, "HEAD")
        print(f"[release] ветка {rel_branch} указывает на {git('rev-parse', '--short', 'HEAD')}")

    if args.no_push:
        print("[release] отправка отключена (--no-push)")
        return

    ok, msg = git_try("push", "-u", "origin", branch)
    print(f"[release] отправка {branch}: " + ("готово" if ok else f"не удалась — {msg}"))

    if args.release:
        # --force-with-lease: релизная ветка всегда указывает на текущую версию, но чужие
        # изменения на ней перетирать нельзя
        ok, msg = git_try("push", "--force-with-lease", "origin", rel_branch)
        if ok:
            print(f"[release] ветка {rel_branch} отправлена — сборка и выкладка идут в Actions")
        else:
            print(f"[release] {rel_branch} отправить не удалось — {msg}")


if __name__ == "__main__":
    main()
