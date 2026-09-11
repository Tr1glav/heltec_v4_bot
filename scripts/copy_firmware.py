Import("env")
import shutil
import os
from datetime import datetime

def copy_firmware(source, target, env):
    # Получаем путь к собранному файлу (это "$BUILD_DIR/firmware.bin")
    firmware_path = str(target[0])

    # Определяем имя платы (окружения)
    board_name = env.get("PIOENV")

    # Создаем целевую папку
    output_dir = "firmware_output"
    os.makedirs(output_dir, exist_ok=True)

    # Формируем метку времени: ГГГГ-ММ-ДД_ЧЧ-ММ-СС
    timestamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")

    # Формируем новое имя файла: <env>_<timestamp>.bin
    new_filename = f"{board_name}_{timestamp}.bin"
    destination = os.path.join(output_dir, new_filename)

    # Копируем файл
    shutil.copyfile(firmware_path, destination)
    print(f"\n✅ Firmware copied to: {destination}")

    # ===== ОЧИСТКА: оставляем только последние 3 версии ДЛЯ ЭТОЙ ПЛАТЫ =====
    KEEP = 3
    prefix = f"{board_name}_"

    # Собираем все .bin этой платы, сортируем по имени (= по времени,
    # т.к. формат ISO-подобный: YYYY-MM-DD_HH-MM-SS даёт лексикографический порядок)
    candidates = sorted(
        f for f in os.listdir(output_dir)
        if f.startswith(prefix) and f.endswith(".bin")
    )

    # Всё, что старше последних KEEP — удаляем
    to_delete = candidates[:-KEEP] if len(candidates) > KEEP else []
    for old in to_delete:
        old_path = os.path.join(output_dir, old)
        try:
            os.remove(old_path)
            print(f"🗑️  Removed old firmware: {old}")
        except OSError as e:
            print(f"⚠️  Failed to remove {old}: {e}")

    print(f"📦 Kept {min(len(candidates), KEEP)} firmware(s) for '{board_name}'\n")

# Регистрируем функцию как пост-действие для файла firmware.bin
env.AddPostAction("$BUILD_DIR/firmware.bin", copy_firmware)