#!/usr/bin/env python3
"""
Скрипт для сбора всех файлов проекта в один текстовый файл.
Использование: python collect_project.py [output_file]
По умолчанию output_file = "Full_project.txt"
"""

import os
import sys
from pathlib import Path

# Расширения файлов, которые нужно собирать
INCLUDE_EXTENSIONS = {
    '.ini',      # PlatformIO конфигурация
    '.h',        # Заголовочные файлы
    '.hpp',      # Заголовочные файлы C++
    '.c',        # Исходные файлы C
    '.cpp',      # Исходные файлы C++
}

# Директории для поиска
SEARCH_DIRS = ['src', 'include']

# Директории, которые нужно игнорировать
IGNORE_DIRS = {
    '.pio',      # PlatformIO build
    '.vscode',   # VS Code
    '.git',      # Git
    'build',     # Build output
    'lib',       # Библиотеки (если не нужны)
    'test',      # Тесты (если не нужны)
}

# Файлы, которые нужно игнорировать
IGNORE_FILES = {
    'Full_project.txt',
    'collect_project.py',
    '.gitignore',
    'README.md',
    'AGENTS.md',
    'CHANGELOG.md',
}

# Корневые файлы, которые нужно включить (лежат в корне проекта)
ROOT_FILES = ['platformio.ini']


def should_ignore_dir(dir_name: str) -> bool:
    """Проверить, нужно ли игнорировать директорию."""
    return dir_name in IGNORE_DIRS or dir_name.startswith('.')


def should_ignore_file(file_name: str) -> bool:
    """Проверить, нужно ли игнорировать файл."""
    return file_name in IGNORE_FILES


def get_file_extension(file_path: Path) -> str:
    """Получить расширение файла в нижнем регистре."""
    return file_path.suffix.lower()


def write_file_header(f, file_path: Path, relative_path: str):
    """Записать заголовок файла."""
    f.write(f"\n// START {relative_path}\n")
    f.write(f"// {'=' * 70}\n")


def write_file_footer(f, file_path: Path, relative_path: str):
    """Записать завершающий комментарий файла."""
    f.write(f"// END {relative_path}\n\n")


def process_file(f, file_path: Path, project_root: Path):
    """Обработать один файл и записать его содержимое."""
    try:
        with open(file_path, 'r', encoding='utf-8', errors='replace') as source_file:
            content = source_file.read()
    except Exception as e:
        print(f"  ОШИБКА при чтении {file_path}: {e}")
        return

    relative_path = file_path.relative_to(project_root).as_posix()
    write_file_header(f, file_path, relative_path)
    f.write(content)
    
    # Добавляем перенос строки в конце файла, если его нет
    if content and not content.endswith('\n'):
        f.write('\n')
    
    write_file_footer(f, file_path, relative_path)
    print(f"  ✓ {relative_path}")


def collect_root_files(f, project_root: Path):
    """Собрать файлы из корня проекта."""
    for file_name in ROOT_FILES:
        file_path = project_root / file_name
        if file_path.exists() and file_path.is_file():
            process_file(f, file_path, project_root)


def collect_directory(f, directory: Path, project_root: Path):
    """Рекурсивно собрать все файлы из директории."""
    if not directory.exists():
        return
    
    for item in sorted(directory.iterdir()):
        if item.is_file():
            ext = get_file_extension(item)
            if ext in INCLUDE_EXTENSIONS and not should_ignore_file(item.name):
                process_file(f, item, project_root)
        elif item.is_dir():
            if not should_ignore_dir(item.name):
                collect_directory(f, item, project_root)


def main():
    # Определяем корень проекта (текущая директория)
    project_root = Path.cwd()
    
    # Проверяем, что это PlatformIO проект
    pio_ini = project_root / 'platformio.ini'
    if not pio_ini.exists():
        print("ОШИБКА: platformio.ini не найден. Запустите скрипт в корне PlatformIO проекта.")
        sys.exit(1)
    
    # Имя выходного файла
    output_file = sys.argv[1] if len(sys.argv) > 1 else 'Full_project.txt'
    output_path = project_root / output_file
    
    print(f"=== Сборка проекта в {output_path} ===\n")
    print(f"Корень проекта: {project_root}")
    print(f"Директории поиска: {SEARCH_DIRS}")
    print(f"Расширения: {', '.join(sorted(INCLUDE_EXTENSIONS))}")
    print()
    
    with open(output_path, 'w', encoding='utf-8') as f:
        # Заголовок файла
        f.write("// " + "=" * 70 + "\n")
        f.write("// Собранный проект: Bluetooth_connect_v2\n")
        f.write(f"// Дата сборки: {Path.cwd()}\n")
        f.write("// " + "=" * 70 + "\n")
        
        # 1. Файлы из корня проекта
        print("Сбор корневых файлов:")
        collect_root_files(f, project_root)
        
        # 2. Файлы из директорий поиска
        for dir_name in SEARCH_DIRS:
            dir_path = project_root / dir_name
            if dir_path.exists():
                print(f"\nСбор директории: {dir_name}/")
                collect_directory(f, dir_path, project_root)
            else:
                print(f"\nДиректория не найдена: {dir_name}/")
        
        # 3. Дополнительные файлы (если есть)
        extra_dirs = ['src/common']
        for dir_name in extra_dirs:
            dir_path = project_root / dir_name
            if dir_path.exists():
                print(f"\nСбор дополнительной директории: {dir_name}/")
                collect_directory(f, dir_path, project_root)
    
    # Статистика
    output_size = output_path.stat().st_size
    print(f"\n=== Готово! ===")
    print(f"Выходной файл: {output_path}")
    print(f"Размер: {output_size:,} байт ({output_size / 1024:.1f} KB)")
    
    # Подсчёт строк
    with open(output_path, 'r', encoding='utf-8') as f:
        line_count = sum(1 for _ in f)
    print(f"Строк: {line_count:,}")


if __name__ == '__main__':
    main()