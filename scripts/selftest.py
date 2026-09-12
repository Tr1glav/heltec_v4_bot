#!/usr/bin/env python3
"""Проверки прошивки на ПК, без железа.

Что делает:
  * вырезает из исходников настоящие функции (CRC, jsonEscape, buildPingReply, rawBuildFrame),
    собирает их хостовым компилятором с санитайзерами и гоняет на граничных данных —
    так ловятся переполнения буферов, которые на плате проявились бы падением;
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
    src = (ROOT / rel).read_text(encoding="utf-8")
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


def page_js_test():
    src = (ROOT / "lib/meshcore/src/ota.cpp").read_text(encoding="utf-8")
    js = src[src.index('R"JS(') + 5: src.index(')JS"')]
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
    otaz_test()
    page_js_test()
    print()
    if failures:
        print("ПРОВАЛЕНО: " + ", ".join(failures))
        sys.exit(1)
    print("все проверки прошли")
