#!/usr/bin/env python3
from pathlib import Path
import re

modules = [
    'src/simulator_task.cpp',
    'src/calculator.cpp',
    'src/kline_task.cpp',
    'src/climate.cpp',
    'src/storage_task.cpp',
]

for f in modules:
    path = Path(f)
    if path.exists():
        with open(path, 'r', encoding='utf-8') as file:
            content = file.read()
        
        # Проверяем, есть ли case CMD_OTA_START
        if 'case CMD_OTA_START:' not in content:
            # Добавляем перед default
            content = re.sub(
                r'(default:\s*return false;)',
                'case CMD_OTA_START:\n            return true;  // Завершить задачу при OTA\n        \1',
                content
            )
        else:
            # Исправляем return false на return true
            content = re.sub(
                r'case CMD_OTA_START:\s*return false;',
                'case CMD_OTA_START:\n            return true;  // Завершить задачу при OTA',
                content
            )
        
        with open(path, 'w', encoding='utf-8') as file:
            file.write(content)
        print(f"✓ {f}")