# extra_script.py — Переименование firmware.bin после сборки
# Формат: firmCar_<VERSION>.bin
# Пример: firmCar_6_6_0.bin

Import("env")
import os
import shutil
import re

def rename_firmware(source, target, env):
    # Получаем путь к firmware.bin
    firmware_path = os.path.join(env.get("PROJECT_BUILD_DIR"), env.get("PIOENV"), "firmware.bin")

    # Извлекаем версию из app_config.h напрямую
    version = "unknown"
    config_path = os.path.join(env.get("PROJECT_DIR"), "include", "app_config.h")
    if os.path.exists(config_path):
        with open(config_path, 'r', encoding='utf-8') as f:
            content = f.read()
            match = re.search(r'#define\s+FW_VERSION_STR\s+"([^"]+)"', content)
            if match:
                version = match.group(1).replace('.', '_')

    # Формируем новое имя
    new_name = f"firmCar_{version}.bin"
    new_path = os.path.join(env.get("PROJECT_BUILD_DIR"), env.get("PIOENV"), new_name)

    # Копируем с новым именем
    if os.path.exists(firmware_path):
        shutil.copy2(firmware_path, new_path)
        print(f"\n[EXTRA] Firmware renamed: {new_name}")
        print(f"[EXTRA] Path: {new_path}\n")

env.AddPostAction("$BUILD_DIR/firmware.bin", rename_firmware)
