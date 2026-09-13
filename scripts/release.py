#!/usr/bin/env python3
"""Фиксация версии в git: ветка v<версия>, develop переводится на неё.

Замысел: каждая выпущенная версия остаётся отдельной веткой, а develop всегда указывает
на последнюю. Ветка main не трогается — в неё вливаешь сам, когда посчитаешь нужным.

Запуск вручную:
    python3 scripts/release.py                 коммит, ветка, develop, отправка на origin
    python3 scripts/release.py --no-push       то же, но без отправки
    python3 scripts/release.py --dry-run       только показать, что будет сделано
    python3 scripts/release.py -m "текст"      своё описание коммита

Из сборки вызывается автоматически, но только при RELEASE=1:
    RELEASE=1 pio run -e heltec_v3_mqtt
Обычная сборка в git ничего не пишет.
"""
import argparse
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Пути, которых в коммите быть не должно ни при каких обстоятельствах. .gitignore их и так
# исключает, но здесь цена ошибки — утечка паролей и ключей каналов в историю, поэтому
# проверяем ещё раз перед фиксацией.
FORBIDDEN = ("secrets.ini", "secrets.json", "firmware_output/", "build_info.h")


def git(*args, check=True, capture=True):
    r = subprocess.run(["git", *args], cwd=str(ROOT), check=False, text=True,
                       capture_output=capture)
    if check and r.returncode != 0:
        sys.exit(f"git {' '.join(args)}: {(r.stderr or r.stdout).strip()}")
    return (r.stdout or "").strip()


def version():
    with open(ROOT / "version.txt") as fh:
        return fh.read().split()[0]


def branch_exists(name):
    return subprocess.run(["git", "rev-parse", "--verify", "--quiet", name],
                          cwd=str(ROOT), capture_output=True).returncode == 0


def staged_files():
    return [l for l in git("diff", "--cached", "--name-only").splitlines() if l]


def main():
    ap = argparse.ArgumentParser(description="ветка на версию и develop на неё")
    ap.add_argument("--no-push", action="store_true", help="не отправлять на origin")
    ap.add_argument("--dry-run", action="store_true", help="только показать план")
    ap.add_argument("-m", "--message", help="описание коммита")
    ap.add_argument("--quiet-if-exists", action="store_true",
                    help="молча выйти, если ветка этой версии уже есть (для вызова из сборки)")
    args = ap.parse_args()

    ver = version()
    branch = f"v{ver}"
    current = git("rev-parse", "--abbrev-ref", "HEAD")

    if branch_exists(branch):
        msg = f"ветка {branch} уже существует — версия {ver} уже зафиксирована"
        if args.quiet_if_exists:
            print(f"[release] {msg}, пропускаю")
            return
        sys.exit(f"[release] {msg}")

    git("add", "-A")
    files = staged_files()
    if not files:
        print("[release] изменений нет, фиксировать нечего")
        git("reset", check=False)
        return

    bad = [f for f in files if any(f.startswith(p) or f.endswith(p) for p in FORBIDDEN)]
    if bad:
        git("reset", check=False)
        sys.exit("[release] в коммит попали закрытые файлы, остановлено: " + ", ".join(bad))

    message = args.message or f"v{ver}"
    print(f"[release] версия {ver}, файлов в коммите: {len(files)}, текущая ветка: {current}")
    print(f"[release] план: ветка {branch} <- коммит «{message}», develop -> {branch}"
          + ("" if args.no_push else ", затем отправка на origin"))

    if args.dry_run:
        print("[release] пробный прогон, ничего не меняю")
        for f in files[:20]:
            print("   ", f)
        if len(files) > 20:
            print(f"    ... и ещё {len(files) - 20}")
        git("reset", check=False)
        return

    git("switch", "-c", branch)
    git("commit", "-m", message)
    head = git("rev-parse", "HEAD")
    print(f"[release] коммит {head[:8]} в ветке {branch}")

    # develop всегда указывает на последнюю версию
    git("branch", "-f", "develop", branch)
    git("switch", "develop")
    print("[release] develop переведён на " + branch)

    if args.no_push:
        print("[release] отправка отключена (--no-push)")
        return

    r = subprocess.run(["git", "push", "origin", branch, "develop"],
                       cwd=str(ROOT), text=True, capture_output=True,
                       env={**os.environ, "GIT_TERMINAL_PROMPT": "0"})
    if r.returncode == 0:
        print(f"[release] отправлено на origin: {branch}, develop")
    else:
        err = (r.stderr or r.stdout).strip().splitlines()
        print("[release] отправить не удалось:", err[-1] if err else "неизвестная ошибка")
        print("[release] коммит и ветки на месте — отправьте вручную:"
              f" git push origin {branch} develop")


if __name__ == "__main__":
    main()
