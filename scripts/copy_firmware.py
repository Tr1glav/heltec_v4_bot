Import("env")
import shutil
import os
import struct
import zlib
from datetime import datetime

# Сколько последних версий хранить для каждой платы
KEEP = 3


def is_sensor_env(env):
    for d in env.get("CPPDEFINES", []):
        name = d[0] if isinstance(d, (list, tuple)) else d
        if name == "SENSOR_NODE":
            return True
    return False


def cleanup(output_dir, prefix, ext):
    # Сортировка по имени = по времени: YYYY-MM-DD_HH-MM-SS упорядочен лексикографически
    candidates = sorted(
        f for f in os.listdir(output_dir)
        if f.startswith(prefix) and f.endswith(ext)
    )
    for old in candidates[:-KEEP]:
        try:
            os.remove(os.path.join(output_dir, old))
            print(f"🗑️  Removed old firmware: {old}")
        except OSError as e:
            print(f"⚠️  Failed to remove {old}: {e}")
    return min(len(candidates), KEEP)


def copy_firmware(source, target, env):
    # Путь к собранному файлу ("$BUILD_DIR/firmware.bin")
    firmware_path = str(target[0])
    board_name = env.get("PIOENV")

    output_dir = "firmware_output"
    os.makedirs(output_dir, exist_ok=True)

    # Метка времени: ГГГГ-ММ-ДД_ЧЧ-ММ-СС
    timestamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    prefix = f"{board_name}_"

    destination = os.path.join(output_dir, f"{prefix}{timestamp}.bin")
    shutil.copyfile(firmware_path, destination)
    print(f"\n✅ Firmware copied to: {destination}")
    kept = cleanup(output_dir, prefix, ".bin")

    # Сжатая копия для mesh OTA сенсоров, формат OTA_Z_* в lib/meshcore/include/config.h:
    # [OTAZ][размер образа 4B LE][CRC32 образа 4B LE][zlib]
    if is_sensor_env(env):
        with open(firmware_path, "rb") as fh:
            raw = fh.read()
        packed = (b"OTAZ" + struct.pack("<II", len(raw), zlib.crc32(raw) & 0xFFFFFFFF)
                  + zlib.compress(raw, 9))
        zpath = os.path.join(output_dir, f"{prefix}{timestamp}.otaz")
        with open(zpath, "wb") as fh:
            fh.write(packed)
        print(f"🗜️  Mesh OTA (сжато): {zpath} ({len(raw)} -> {len(packed)} байт)")
        cleanup(output_dir, prefix, ".otaz")

    print(f"📦 Kept {kept} firmware(s) for '{board_name}'\n")


# Регистрируем функцию как пост-действие для файла firmware.bin
env.AddPostAction("$BUILD_DIR/firmware.bin", copy_firmware)
