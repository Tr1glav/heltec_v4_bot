#!/usr/bin/env python3
"""Проверки прошивки на ПК, без железа.

Что делает:
  * вырезает из исходников настоящие функции (CRC, jsonEscape, buildPingReply, rawBuildFrame),
    собирает их хостовым компилятором с санитайзерами и гоняет на граничных данных —
    так ловятся переполнения буферов, которые на плате проявились бы падением;
  * проверяет сканер маркера платы: маркер должен находиться при любой нарезке образа
    на куски, чужой код платы — отвергаться, отсутствие маркера — не считаться отказом;
  * проверяет формат .otaz (заголовок, CRC32, распаковка кусками по 240 Б, как на сенсоре);
  * проверяет синтаксис JavaScript страницы OTA.

Запуск: python3 scripts/selftest.py
Нужен g++; node — по желанию (без него проверка JS пропускается).
"""
import pathlib
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
failures = []


def check(name, ok, detail=""):
    print(("OK   " if ok else "FAIL ") + name + ((" — " + detail) if detail and not ok else ""))
    if not ok:
        failures.append(name)


def grab(rel, signature):
    """Вырезает функцию из исходника по началу сигнатуры, считая фигурные скобки."""
    # Путь — подсказка, а не требование: файлы переезжают при разборке на модули, и
    # жёсткая привязка ломает проверки на ровном месте. Не нашлось по подсказке — ищем
    # сигнатуру по всем исходникам прошивки.
    path = ROOT / rel
    if signature not in path.read_text(encoding="utf-8"):
        for cand in sorted((ROOT / "lib/meshcore/src").glob("*.cpp")) + \
                    sorted((ROOT / "src").glob("*.cpp")):
            if signature in cand.read_text(encoding="utf-8"):
                path = cand
                break
        else:
            raise RuntimeError("не найдена функция " + signature)
    src = path.read_text(encoding="utf-8")
    start = src.index(signature)
    depth = 0
    for i in range(src.index("{", start), len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[start:i + 1]
    raise RuntimeError("не найден конец функции " + signature)


HOST_MAIN = r"""
int main() {
    // buildPingReply: путь приходит из эфира, до 63 хопов по 4 байта
    uint8_t path[63 * 4];
    for (size_t i = 0; i < sizeof(path); i++) path[i] = (uint8_t)i;
    for (int hs = 1; hs <= 4; hs++) {
        for (int hops = 0; hops <= 63; hops++) {
            char out[100];
            memset(out, 'X', sizeof(out));
            buildPingReply(out, sizeof(out), path, hops, hs);
            if (strnlen(out, sizeof(out)) >= sizeof(out)) {
                printf("buildPingReply: строка не завершена hs=%d hops=%d\n", hs, hops);
                return 1;
            }
        }
    }
    // jsonEscape: произвольный текст не должен вылезать за выходной буфер
    for (int len = 0; len < 200; len++) {
        char in[256], out[64];
        for (int i = 0; i < len; i++) in[i] = (char)(1 + rand() % 254);
        in[len] = 0;
        memset(out, 'X', sizeof(out));
        jsonEscape(in, out, sizeof(out));
        if (strnlen(out, sizeof(out)) >= sizeof(out)) {
            printf("jsonEscape: строка не завершена len=%d\n", len);
            return 1;
        }
    }
    // rawBuildFrame: кадр обязан влезать в буфер OTA_RAW_FRAME_MAX и в лимит SX1262
    uint8_t data[OTA_RAW_CHUNK_BYTES + 2], frame[OTA_RAW_FRAME_MAX];
    memset(data, 0xA5, sizeof(data));
    for (int n = 0; n <= (int)sizeof(data); n++) {
        int f = rawBuildFrame(frame, 0x02, 12345, data, n);
        if (f != 9 + n) { printf("rawBuildFrame: длина %d при n=%d\n", f, n); return 1; }
        if (f > 255) { printf("rawBuildFrame: кадр %d > 255 байт\n", f); return 1; }
    }
    const char* s = "meshcore";
    printf("crc32=%08X\n", (unsigned)~crc32_upd(0xFFFFFFFF, (const uint8_t*)s, strlen(s)));
    printf("crc16=%04X\n", crc16buf((const uint8_t*)s, strlen(s)));
    return 0;
}
"""


def host_functions_test():
    if not shutil.which("g++"):
        print("SKIP g++ не найден — проверки границ буферов пропущены")
        return
    code = (
        "#include <cstdint>\n#include <cstdio>\n#include <cstring>\n#include <cstdlib>\n"
        "#define OTA_RAW_CHUNK_BYTES 240\n"
        "#define OTA_RAW_FRAME_MAX (11 + OTA_RAW_CHUNK_BYTES)\n"
        "#define RAW_MAGIC0 0xBE\n#define RAW_MAGIC1 0xEF\n"
        + grab("lib/meshcore/src/crypto.cpp", "uint16_t crc16buf(") + "\n"
        + grab("lib/meshcore/src/crypto.cpp", "uint32_t crc32_upd(") + "\n"
        + grab("lib/meshcore/src/crypto.cpp", "void jsonEscape(") + "\n"
        + grab("lib/meshcore/src/mesh.cpp", "void buildPingReply(") + "\n"
        + grab("lib/meshcore/src/ota.cpp", "int rawBuildFrame(") + "\n"
        + HOST_MAIN
    )
    with tempfile.TemporaryDirectory() as tmp:
        src = pathlib.Path(tmp) / "t.cpp"
        exe = pathlib.Path(tmp) / "t"
        src.write_text(code, encoding="utf-8")
        build = subprocess.run(
            ["g++", "-std=c++17", "-fsanitize=address,undefined", "-g", str(src), "-o", str(exe)],
            capture_output=True, text=True)
        if build.returncode != 0:
            check("сборка хостового теста", False, build.stderr.strip()[:400])
            return
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        check("границы буферов (buildPingReply, jsonEscape, rawBuildFrame)",
              run.returncode == 0, (run.stdout + run.stderr).strip()[:400])
        got = dict(re.findall(r"(crc\d+)=([0-9A-F]+)", run.stdout))
        expect = "%08X" % (zlib.crc32(b"meshcore") & 0xFFFFFFFF)
        check("crc32 совпадает с zlib", got.get("crc32") == expect,
              "получено %s, ожидалось %s" % (got.get("crc32"), expect))


MARKER_PRELUDE = (
    "#include <cstdint>\n#include <cstdio>\n#include <cstring>\n"
    "#define BOARD_CODE \"h3\"\n"
    "#define FW_MARK_PREFIX \"MBFW:\"\n"
    "#define min(a,b) ((a)<(b)?(a):(b))\n"
    "struct FwScan { char carry[40]; uint8_t carryLen; bool mine; char other[12]; };\n"
)

MARKER_MAIN = r"""
static void feedAll(FwScan* s, const char* img, size_t n, size_t chunk) {
    fwScanReset(s);
    for (size_t i = 0; i < n; i += chunk) {
        size_t k = (n - i < chunk) ? (n - i) : chunk;
        fwScanFeed(s, (const uint8_t*)img + i, k);
    }
}

int main() {
    char img[4096];
    FwScan s;
    // маркер своей платы обязан находиться при любом размере куска, в том числе
    // когда он лёг на границу двух кусков — это и есть главный риск сканера
    const char* mine = "MBFW:" BOARD_CODE ":1.0.27";
    for (size_t at = 0; at + 64 < sizeof(img); at += 37) {
        memset(img, 0xA5, sizeof(img));
        memcpy(img + at, mine, strlen(mine));
        for (size_t chunk = 1; chunk <= 64; chunk++) {
            feedAll(&s, img, sizeof(img), chunk);
            if (fwScanVerdict(&s) != 1) {
                printf("свой маркер не найден: смещение %zu, кусок %zu\n", at, chunk);
                return 1;
            }
        }
    }
    // образ чужой платы должен быть отвергнут с указанием её кода
    memset(img, 0xA5, sizeof(img));
    memcpy(img + 700, "MBFW:zz9:1.0.27", 15);
    feedAll(&s, img, sizeof(img), 512);
    if (fwScanVerdict(&s) != -1 || strcmp(s.other, "zz9") != 0) {
        printf("чужая плата не распознана: verdict=%d other=%s\n", fwScanVerdict(&s), s.other);
        return 1;
    }
    // без маркера и голый префикс без кода — «неизвестная» прошивка, а не отказ
    memset(img, 0xA5, sizeof(img));
    feedAll(&s, img, sizeof(img), 512);
    if (fwScanVerdict(&s) != 0) { printf("образ без маркера принят за чужой\n"); return 1; }
    memcpy(img + 100, "MBFW:", 6);
    feedAll(&s, img, sizeof(img), 512);
    if (fwScanVerdict(&s) != 0) { printf("голый префикс принят за маркер\n"); return 1; }
    printf("ok\n");
    return 0;
}
"""


def marker_scan_test():
    if not shutil.which("g++"):
        print("SKIP g++ не найден — сканер маркера платы не проверен")
        return
    code = (MARKER_PRELUDE
            + grab("lib/meshcore/src/ota.cpp", "void fwScanReset(") + "\n"
            + grab("lib/meshcore/src/ota.cpp", "static void fwScanBuf(") + "\n"
            + grab("lib/meshcore/src/ota.cpp", "void fwScanFeed(") + "\n"
            + grab("lib/meshcore/src/ota.cpp", "int fwScanVerdict(") + "\n"
            + MARKER_MAIN)
    with tempfile.TemporaryDirectory() as tmp:
        src = pathlib.Path(tmp) / "m.cpp"
        exe = pathlib.Path(tmp) / "m"
        src.write_text(code, encoding="utf-8")
        build = subprocess.run(
            ["g++", "-std=c++17", "-fsanitize=address,undefined", "-g", str(src), "-o", str(exe)],
            capture_output=True, text=True)
        if build.returncode != 0:
            check("сборка теста маркера платы", False, build.stderr.strip()[:400])
            return
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        check("маркер платы: поиск в потоке, в том числе на границах кусков",
              run.returncode == 0, (run.stdout + run.stderr).strip()[:400])


def otaz_test():
    raw = bytes(random.getrandbits(8) for _ in range(50000))
    packed = (b"OTAZ" + struct.pack("<II", len(raw), zlib.crc32(raw) & 0xFFFFFFFF)
              + zlib.compress(raw, 9))
    size, crc = struct.unpack("<II", packed[4:12])
    # сенсор скармливает распаковщику куски по 240 байт, как они приходят в кадрах
    d = zlib.decompressobj()
    body = packed[12:]
    out = b"".join(d.decompress(body[i:i + 240]) for i in range(0, len(body), 240)) + d.flush()
    check("формат .otaz: заголовок", packed[:4] == b"OTAZ" and size == len(raw))
    check("формат .otaz: CRC32 образа", crc == zlib.crc32(raw) & 0xFFFFFFFF)
    check("формат .otaz: распаковка кусками по 240 Б", out == raw)
    # Бот дописывает в эфир OTA_Z_TAIL_PAD нулей после потока: без них распаковщик узла
    # придерживает хвост образа, и приём падает с «size mismatch». Длину берём из config.h,
    # чтобы проверка и прошивка не разъехались, и убеждаемся, что лишний вход безвреден.
    cfg = (ROOT / "lib/meshcore/include/config.h").read_text(encoding="utf-8")
    m = re.search(r"#define\s+OTA_Z_TAIL_PAD\s+(\d+)", cfg)
    check("формат .otaz: объявлен хвост нулей", m is not None)
    padded = body + bytes(int(m.group(1)) if m else 0)
    dp = zlib.decompressobj()
    outp = b"".join(dp.decompress(padded[i:i + 240]) for i in range(0, len(padded), 240)) + dp.flush()
    check("формат .otaz: хвостовые нули не портят образ", outp == raw)


def page_js_test():
    js = (ROOT / "web/app.js").read_text(encoding="utf-8")
    check("JavaScript страницы непустой", len(js) > 1000)
    if not shutil.which("node"):
        print("SKIP node не найден — синтаксис страницы не проверен")
        return
    with tempfile.TemporaryDirectory() as tmp:
        path = pathlib.Path(tmp) / "page.js"
        path.write_text(js, encoding="utf-8")
        run = subprocess.run(["node", "--check", str(path)], capture_output=True, text=True)
        check("синтаксис JavaScript страницы", run.returncode == 0, run.stderr.strip()[:400])


if __name__ == "__main__":
    host_functions_test()
    marker_scan_test()
    otaz_test()
    page_js_test()
    print()
    if failures:
        print("ПРОВАЛЕНО: " + ", ".join(failures))
        sys.exit(1)
    print("все проверки прошли")
