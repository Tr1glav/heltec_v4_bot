#!/usr/bin/env python3
"""Прошивка и настройка устройства по USB из локального secrets.json.

Прошивка больше не содержит ни паролей, ни ключей каналов, ни имени узла — всё это
живёт в NVS устройства и задаётся через его консоль. Этот скрипт делает то же самое,
что руками в мониторе порта, только без опечаток: читает secrets.json, отправляет
команды "set", сохраняет и перезагружает, а потом проверяет, что записалось.

Секреты в вывод не печатаются никогда: вместо значений показывается длина.

Команды:
  python3 scripts/provision.py import-ini          создать secrets.json из старого secrets.ini
  python3 scripts/provision.py list                показать последовательные порты
  python3 scripts/provision.py show <устройство>   что будет записано (значения скрыты)
  python3 scripts/provision.py config <устройство> только настроить (без прошивки)
  python3 scripts/provision.py flash <устройство>  собрать, прошить по USB и настроить
  python3 scripts/provision.py verify <устройство> прочитать настройки с платы и сверить

Устройство — ключ из раздела "devices" файла secrets.json.
Порт: берётся из "port" устройства, иначе --port, иначе автоопределение.
"""
import argparse
import json
import os
import pathlib
import re
import subprocess
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("нужен pyserial: pip install pyserial")

ROOT = pathlib.Path(__file__).resolve().parent.parent
SECRETS = ROOT / "secrets.json"
BAUD = 115200

# Какие поля отправлять устройству в зависимости от роли. Сенсору WiFi и MQTT не нужны:
# он живёт только на радио, и лишние поля только занимали бы место в NVS.
RADIO_FIELDS = ["lora_freq", "lora_bw", "lora_sf", "lora_cr", "lora_tx",
                "lora_pre", "lora_sync", "tz", "disp_bri", "vext_on"]
ROLE_FIELDS = {
    "bot": ["name", "wifi_ssid", "wifi_pass", "mqtt_host", "mqtt_port",
            "mqtt_user", "mqtt_pass", "prv_name", "prv_key", "sns_name", "sns_key",
            "tx_ch"] + RADIO_FIELDS,
    "sensor": ["name", "sns_name", "sns_key"] + RADIO_FIELDS,
}
SECRET_FIELDS = {"wifi_pass", "mqtt_pass", "prv_key", "sns_key"}

# Соответствие старых build-флагов полям конфига — для import-ini
INI_MAP = {
    "WIFI_SSID": "wifi_ssid", "WIFI_PASS": "wifi_pass",
    "MQTT_BROKER": "mqtt_host", "MQTT_PORT": "mqtt_port",
    "MQTT_USER": "mqtt_user", "MQTT_PASS": "mqtt_pass",
    "PRIVATE_CHANNEL_NAME": "prv_name", "PRIVATE_CHANNEL_KEY": "prv_key",
    "SENSOR_CHANNEL_NAME": "sns_name", "SENSOR_CHANNEL_KEY": "sns_key",
    "TX_CHANNEL": "tx_ch",
}


def mask(field, value):
    if field in SECRET_FIELDS and value:
        return f"(задано, {len(str(value).encode())} симв.)"
    return str(value) if value != "" else "(пусто)"


def load_secrets():
    if not SECRETS.exists():
        sys.exit(f"нет {SECRETS.name} — создайте его из secrets.example.json "
                 f"или выполните: python3 scripts/provision.py import-ini")
    data = json.loads(SECRETS.read_text(encoding="utf-8"))
    if "devices" not in data:
        sys.exit("в secrets.json нет раздела \"devices\"")
    return data


def device_config(data, key):
    """Итоговый набор полей: общие значения, поверх них — что задано у устройства.

    Устройство выбирается по имени узла: ключ в "devices" и есть имя, которое уедет
    в настройки платы. Для совместимости понимаем и явно заданное поле "name".
    """
    devices = data["devices"]
    dev = devices.get(key)
    if dev is None:
        for k, v in devices.items():
            if isinstance(v, dict) and v.get("name") == key:
                dev, key = v, k
                break
    if dev is None:
        sys.exit(f"устройства '{key}' нет в secrets.json; есть: "
                 + ", ".join(sorted(devices)))
    role = dev.get("role")
    if role not in ROLE_FIELDS:
        sys.exit(f"у устройства '{key}' роль должна быть bot или sensor")
    merged = dict(data.get("common", {}))
    merged["name"] = dev.get("name", key)   # имя узла = ключ, если явно не задано иное
    merged.update({k: v for k, v in dev.items() if k not in ("role", "env", "port")})
    fields = dev.get("fields") or ROLE_FIELDS[role]
    # Только то, что реально задано в secrets.json: отсутствующее поле оставляем на плате
    # как есть. Иначе незаполненные параметры радио обнулили бы рабочие настройки.
    values = {f: merged[f] for f in fields if f in merged}
    if not values:
        sys.exit(f"для устройства '{key}' в secrets.json нет ни одного известного поля")
    return dev, values


def usb_ports():
    """Только USB-платы: у встроенных портов материнской платы (/dev/ttyS*) нет VID."""
    return [p for p in list_ports.comports() if p.vid is not None]


def pick_port(dev, args):
    if dev.get("port"):
        return dev["port"]
    if args.port:
        return args.port
    ports = [p.device for p in usb_ports()]
    if len(ports) == 1:
        print(f"[порт] автоопределение: {ports[0]}")
        return ports[0]
    if not ports:
        sys.exit("USB-портов не найдено — подключите плату")
    sys.exit("портов несколько (" + ", ".join(ports) + "), укажите --port")


def open_port(port):
    """Открывает порт, не дёргая плату сбросом.

    На платах с CP2102 линии RTS и DTR заведены на EN и GPIO0 (esptool так и пишет:
    "Hard resetting via RTS pin"). pyserial по умолчанию поднимает обе при открытии, из-за
    чего плата всё время остаётся в ресете и молчит. Поэтому снимаем их до открытия.
    """
    # После сброса узел порта может пропасть и появиться заново уже ПОСЛЕ того, как
    # wait_for_port его увидел, поэтому открываем с повторами, а не один раз.
    deadline = time.time() + 30
    while True:
        try:
            ser = serial.Serial(port, BAUD, timeout=0.3)
            break
        except (serial.SerialException, OSError):
            if time.time() > deadline:
                raise
            time.sleep(0.5)
    # Открытие поднимает RTS, то есть удерживает плату в ресете. Снимаем сразу: плата
    # перезагрузится один раз и дальше будет работать. У псевдотерминалов и части
    # USB-CDC этих линий нет — тогда просто ничего не делаем.
    clear_modem_lines(ser)
    time.sleep(0.4)
    return ser


def clear_modem_lines(ser):
    try:
        ser.dtr = False
        ser.rts = False
    except (OSError, IOError):
        pass


def reset_board(ser):
    """Короткий импульс по RTS: плата перезагружается и печатает баннер."""
    try:
        ser.rts = True
        time.sleep(0.1)
        ser.rts = False
    except (OSError, IOError):
        return
    time.sleep(0.2)
    ser.reset_input_buffer()


def wait_for_port(path, timeout=30.0):
    """После прошивки и перезагрузки USB-порт пропадает и появляется заново."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(path):
            time.sleep(0.7)   # дать системе доинициализировать устройство
            return True
        time.sleep(0.3)
    return False


def drain_until_quiet(ser, quiet=1.0, limit=20.0):
    """Ждёт, пока плата перестанет печатать.

    При открытии порта плата сбрасывается и выводит загрузочный лог, а в нём есть
    печать настроек со СКРЫТЫМИ секретами и строкой "настроено:". Без этой паузы
    ответом на "show all" считался бы баннер, секреты приходили бы маской, и скрипт
    переписывал бы их при каждом запуске.
    """
    end = time.time() + limit
    last = time.time()
    while time.time() < end:
        if ser.read(256):
            last = time.time()
        elif time.time() - last >= quiet:
            return True
    return False


def talk(ser, line, expect, timeout=4.0, echo_field=None):
    """Отправляет строку и ждёт подтверждения. Асинхронные логи радио игнорируются."""
    ser.reset_input_buffer()
    ser.write((line + "\n").encode("utf-8"))
    ser.flush()
    deadline = time.time() + timeout
    buf = ""
    while time.time() < deadline:
        chunk = ser.read(512).decode("utf-8", "replace")
        if not chunk:
            continue
        buf += chunk
        if expect in buf:
            return True, buf
        if "неизвестн" in buf or "нужно:" in buf:
            return False, buf
    return False, buf


def read_for(ser, seconds):
    out = ""
    deadline = time.time() + seconds
    while time.time() < deadline:
        out += ser.read(512).decode("utf-8", "replace")
    return out


def do_config(port, values, quiet=False, force=False):
    print(f"[настройка] порт {port}")
    with open_port(port) as ser:
        ser.reset_input_buffer()
        # Пробный запрос: плата могла ещё грузиться после прошивки, и первая команда
        # ушла бы в никуда. Заодно сразу видно прошивку без консоли настроек.
        # Читаем с секретами, чтобы сверка была точной: пароль той же длины, но другой,
        # иначе молча остался бы старым. В вывод скрипта значения всё равно не попадают.
        drain_until_quiet(ser)   # дать плате договорить загрузочный вывод
        ok, probe = talk(ser, "show all", "настроено:", timeout=15.0)
        if not ok:
            reset_board(ser)   # не отвечает — перезагружаем и ждём баннер
            drain_until_quiet(ser)
            ok, probe = talk(ser, "show all", "настроено:", timeout=15.0)
        if not ok:
            print("  консоль настроек не отвечает: плата не загрузилась "
                  "или на ней прошивка без поддержки настроек")
            return False

        current = parse_show(probe)
        # Если что-то пришло маской, значит мы всё же прочитали не ответ, а баннер —
        # переспрашиваем, иначе секреты будут переписываться впустую.
        if any(kind == "len" for kind, _ in current.values()):
            ok2, probe2 = talk(ser, "show all", "настроено:", timeout=10.0)
            if ok2:
                current = parse_show(probe2)
        pending = {}
        for field, value in values.items():
            value = "" if value is None else str(value)
            have = current.get(field)
            if have is not None and have[0] == "value" and same_value(have[1], value) and not force:
                continue
            pending[field] = value
        if not pending:
            print("  настройки на плате уже совпадают с файлом — ничего не пишем")
            return True
        skipped = len(values) - len(pending)
        if skipped:
            print(f"  совпадает и не трогается полей: {skipped}")

        for field, value in pending.items():
            value = "" if value is None else str(value)
            if value == "":
                ok, buf = talk(ser, f"clear {field}", "очищено")
                action = "очищено"
            else:
                ok, buf = talk(ser, f"set {field} {value}", "задано")
                action = "задано"
            if not ok:
                print(f"  {field}: ОШИБКА — устройство не подтвердило ({action})")
                print("  ответ:", buf.strip().splitlines()[-1] if buf.strip() else "(пусто)")
                return False
            if not quiet:
                print(f"  {field}: {action} {mask(field, value)}")
        ok, buf = talk(ser, "save", "сохранено", timeout=6.0)
        if not ok:
            print("  save: ОШИБКА — настройки не записаны")
            return False
        print("  save: записано в NVS")
        ser.write(b"reboot\n")
        ser.flush()
    return True


def parse_show(text):
    """Разбирает вывод "show": поле -> значение либо длина скрытого значения."""
    out = {}
    for line in text.splitlines():
        m = re.match(r"\s{2,}([a-z_]+)\s+(.+?)\s*$", line)
        if not m:
            continue
        field, raw = m.group(1), m.group(2)
        m2 = re.match(r"\(задано, (\d+) симв\.\)", raw)
        if m2:
            out[field] = ("len", int(m2.group(1)))
        elif raw == "(пусто)":
            out[field] = ("value", "")
        else:
            out[field] = ("value", raw)
    return out


def same_value(got, want):
    """Числа сравниваем как числа: на плате float, и "62.5" против "62.500" — не расхождение."""
    if got == want:
        return True
    try:
        a, b = float(got), float(want)
    except ValueError:
        return False
    return abs(a - b) <= max(1e-6, abs(b) * 1e-6)


def do_verify(port, values):
    print(f"[проверка] порт {port}")
    with open_port(port) as ser:
        ok, buf = talk(ser, "show", "настроено:", timeout=8.0)
        if not ok:
            reset_board(ser)
            ok, buf = talk(ser, "show", "настроено:", timeout=15.0)
        if not ok:
            print("  устройство не ответило на show")
            return False
    got = parse_show(buf)
    bad = 0
    for field, value in values.items():
        value = "" if value is None else str(value)
        have = got.get(field)
        if have is None:
            print(f"  {field}: НЕ ПРОЧИТАНО")
            bad += 1
            continue
        kind, data = have
        if kind == "len":
            # секретное поле: сверяем длину, значение с платы не читаем
            good = data == len(value.encode())
            print(f"  {field}: {'ок' if good else 'РАСХОЖДЕНИЕ'} (на плате {data} симв., "
                  f"ожидалось {len(value.encode())})")
        else:
            good = same_value(data, value)
            if good:
                print(f"  {field}: ок")
            else:
                print(f"  {field}: РАСХОЖДЕНИЕ (на плате {data!r}, ожидалось {value!r})")
        if not good:
            bad += 1
    print("  устройство считает себя настроенным" if "настроено: да" in buf
          else "  ВНИМАНИЕ: устройство считает себя ненастроенным")
    return bad == 0


def cmd_import_ini(args):
    ini = ROOT / "secrets.ini"
    if not ini.exists():
        sys.exit("secrets.ini не найден")
    if SECRETS.exists() and not args.force:
        sys.exit(f"{SECRETS.name} уже существует (--force чтобы перезаписать)")
    common, bot_name, sensor_name = {}, "", ""
    for line in ini.read_text(encoding="utf-8").splitlines():
        m = re.search(r"-D([A-Z_0-9]+)=(.+)", line.strip())
        if not m:
            continue
        name, value = m.group(1), m.group(2).strip().strip("'").strip('"')
        if name == "DEVICE_NAME":
            bot_name = value
        elif name == "DEVICE_NAME_SENSOR":
            sensor_name = value
        elif name in INI_MAP:
            common[INI_MAP[name]] = int(value) if name == "MQTT_PORT" else value
    data = {
        "common": common,
        "devices": {
            "bot": {"role": "bot", "env": "heltec_v3_mqtt", "name": bot_name},
            "sensor": {"role": "sensor", "env": "heltec_v4_3_sensors", "name": sensor_name},
        },
    }
    SECRETS.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    os.chmod(SECRETS, 0o600)
    print(f"создан {SECRETS.name} (права 600), перенесено полей: {len(common) + 2}")
    print("поля:", ", ".join(sorted(common)) + ", name(bot), name(sensor)")


def cmd_list(args):
    ports = usb_ports()
    if not ports:
        print("USB-плат не найдено (встроенные порты /dev/ttyS* не показываются)")
        return
    for p in ports:
        print(f"  {p.device}  {p.description}")


def cmd_show(args):
    data = load_secrets()
    dev, values = device_config(data, args.device)
    print(f"устройство '{args.device}': роль {dev.get('role')}, окружение {dev.get('env')}")
    for field, value in values.items():
        print(f"  {field}: {mask(field, value)}")


def cmd_config(args):
    data = load_secrets()
    dev, values = device_config(data, args.device)
    port = pick_port(dev, args)
    sys.exit(0 if do_config(port, values, force=args.force) else 1)


def cmd_flash(args):
    data = load_secrets()
    dev, values = device_config(data, args.device)
    env = dev.get("env")
    if not env:
        sys.exit(f"у устройства '{args.device}' не задано окружение (env)")
    port = pick_port(dev, args)
    print(f"[сборка] {env} -> {port}")
    rc = subprocess.call(["pio", "run", "-e", env, "-t", "upload", "--upload-port", port],
                         cwd=str(ROOT))
    if rc != 0:
        sys.exit("прошивка не удалась")
    print("[ожидание] порт возвращается после перезагрузки...")
    if not wait_for_port(port):
        sys.exit(f"порт {port} не появился — подключите плату и запустите config")
    time.sleep(2.0)   # дать прошивке пройти инициализацию радио и экрана
    if not do_config(port, values, force=args.force):
        sys.exit(1)
    print("[ожидание] перезагрузка после сохранения...")
    if wait_for_port(port):
        time.sleep(2.0)
        do_verify(port, values)


def cmd_verify(args):
    data = load_secrets()
    dev, values = device_config(data, args.device)
    port = pick_port(dev, args)
    sys.exit(0 if do_verify(port, values) else 1)


def main():
    ap = argparse.ArgumentParser(description="прошивка и настройка устройства из secrets.json")
    ap.add_argument("--port", help="последовательный порт (иначе из secrets.json или автоопределение)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list", help="показать порты").set_defaults(func=cmd_list)
    p = sub.add_parser("import-ini", help="создать secrets.json из secrets.ini")
    p.add_argument("--force", action="store_true", help="перезаписать существующий файл")
    p.set_defaults(func=cmd_import_ini)
    for name, fn, help_text in (("show", cmd_show, "что будет записано"),
                                ("config", cmd_config, "только настроить"),
                                ("flash", cmd_flash, "собрать, прошить и настроить"),
                                ("verify", cmd_verify, "сверить настройки с платой")):
        p = sub.add_parser(name, help=help_text)
        p.add_argument("device", help="имя узла из secrets.json")
        if name in ("config", "flash"):
            p.add_argument("--force", action="store_true",
                           help="записать все поля, даже совпадающие")
        p.set_defaults(func=fn)
    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
