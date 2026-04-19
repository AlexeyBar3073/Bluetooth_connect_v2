#!/usr/bin/env python3
import os
import re

def replace_serial_calls(file_path):
    """Replace Serial.print/printf/println with DBG_PRINTF/DBG_PRINTLN in a file"""
    try:
        with open(file_path, 'r', encoding='utf-8') as f:
            content = f.read()
        
        # Сохраняем оригинальное содержимое для сравнения
        original_content = content
        
        # Заменяем Serial.println(...) на DBG_PRINTLN(...)
        content = re.sub(r'Serial\.println\(', 'DBG_PRINTLN(', content)
        
        # Заменяем Serial.printf(...) на DBG_PRINTF(...)
        content = re.sub(r'Serial\.printf\(', 'DBG_PRINTF(', content)
        
        # Заменяем Serial.print(...) на DBG_PRINTF(...)
        content = re.sub(r'Serial\.print\(', 'DBG_PRINTF(', content)
        
        # Если были изменения, сохраняем файл
        if content != original_content:
            with open(file_path, 'w', encoding='utf-8') as f:
                f.write(content)
            print(f"Обработан файл: {file_path}")
        else:
            print(f"Изменений не требуется: {file_path}")
            
    except Exception as e:
        print(f"Ошибка при обработке файла {file_path}: {e}")

def process_directory(directory):
    """Process all .ino and .cpp files in directory and subdirectories"""
    for root, dirs, files in os.walk(directory):
        for file in files:
            if file.endswith(('.ino', '.cpp', '.h', '.hpp', '.c')):
                file_path = os.path.join(root, file)
                replace_serial_calls(file_path)

if __name__ == "__main__":
    # Обрабатываем текущую директорию и все поддиректории
    current_directory = "."
    process_directory(current_directory)
    print("Замена Serial.print/printf/println на DBG_PRINTF/DBG_PRINTLN завершена.")