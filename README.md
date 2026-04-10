# Bluetooth_connect_v2

**Бортовой компьютер автомобиля на базе ESP32 с модульной Pub/Sub архитектурой, Bluetooth-связью с Android-приложением и энергонезависимой памятью (NVS).**

[![PlatformIO](https://img.shields.io/badge/PlatformIO-ESP32-blue)](https://platformio.org/)
[![Framework](https://img.shields.io/badge/Framework-Arduino-orange)](https://arduino.cc/)
[![RTOS](https://img.shields.io/badge/RTOS-FreeRTOS-green)](https://www.freertos.org/)
[![Version](https://img.shields.io/badge/Version-6.0.0-lightgrey)]()
[![License](https://img.shields.io/badge/License-MIT-yellow)]()

---

## 📋 Содержание

- [Описание](#описание)
- [Возможности](#возможности)
- [Аппаратные требования](#аппаратные-требования)
- [Архитектура](#архитектура)
  - [DataRouter — шина данных](#datarouter--шина-данных)
  - [Диаграмма компонентов](#диаграмма-компонентов)
  - [Поток данных](#поток-данных)
- [Модули системы](#модули-системы)
- [Протокол связи с Android](#протокол-связи-с-android)
- [Установка и сборка](#установка-и-сборка)
- [Конфигурация](#конфигурация)
- [Версионирование](#версионирование)
- [Структура проекта](#структура-проекта)
- [Документация](#документация)
- [Лицензия](#лицензия)

---

## Описание

Bluetooth_connect_v2 — это прошивка бортового компьютера автомобиля на микроконтроллере **ESP32 (WEMOS D1 MINI32)**. Система предоставляет:

- **Виртуальный двигатель** — режим симуляции для отладки без реального автомобиля (кнопка запуска, педаль газа, физика движения)
- **Bluetooth SPP** — беспроводная связь с Android-приложением для отображения телеметрии и управления настройками
- **Энергонезависимая память** — хранение данных пробега, расхода топлива и настроек в NVS ESP32
- **K-Line диагностика** — опрос ЭБУ по протоколу ISO 9141-2 (чтение/сброс кодов ошибок, температуры)
- **OLED дисплей** — вывод основной телеметрии на экран SSD1306 128×64 (I2C)
- **Климат и сервис** — мониторинг температур, давления в шинах, уровня омывающей жидкости

**Архитектура v6.0.0:** DataRouter — типизированные топики, очереди принадлежат модулям, без единого BusMessage. Экономия RAM ~10 КБ, каждый топик использует очередь точно под свой тип данных.

---

## Возможности

### Телеметрия
- **Скорость и RPM** — в реальном времени (10 Гц)
- **Напряжение бортсети** — мониторинг генератора/аккумулятора
- **Мгновенный расход топлива** — л/100км или л/ч (на холостых)
- **Уровень топлива в баке** — расчётный остаток (литры)
- **Передача и селектор АКПП** — текущая передача, позиция селектора, блокировка гидротрансформатора

### Поездки и статистика
- **Одометр (ODO)** — общий пробег автомобиля (не сбрасывается)
- **Trip A / Trip B** — два независимых счётчика пробега с расходом топлива
- **Средний расход** — за текущую поездку и накопленный за всё время
- **Заправка** — команда «полный бак» для перерасчёта остатка топлива

### Диагностика ЭБУ (K-Line)
- **Температуры** — охлаждающей жидкости (OЖ) и масла АКПП
- **Коды ошибок (DTC)** — чтение, расшифровка, сброс
- **Автоопределение протокола** — ISO 9141-2, KWP2000, ISO 14230
- **Сервисные команды** — сброс адаптации АКПП, прокачка жидкости

### Интерфейс
- **OLED дисплей SSD1306** — скорость, RPM, топливо с прогресс-баром, статус Bluetooth и двигателя
- **Android-приложение** — полная телеметрия, настройки, диагностика (отдельный репозиторий)
- **Кнопка и потенциометр** — управление симулятором (запуск двигателя, «педаль газа»)

---

## Аппаратные требования

| Компонент | Модель | Подключение | Примечание |
|-----------|--------|-------------|------------|
| **Микроконтроллер** | ESP32 WEMOS D1 MINI32 | — | Основной модуль |
| **OLED дисплей** | SSD1306 128×64 | I2C (SDA=21, SCL=22) | Опционально |
| **Кнопка запуска** | Тактовая кнопка | GPIO26 (INPUT_PULLUP) | Для симулятора |
| **Потенциометр** | 10 кОм | GPIO33 (ADC 12 бит) | «Педаль газа» для симулятора |
| **K-Line адаптер** | ISO 9141-2 (UART) | Serial2 (TX=17, RX=16) | Опционально, для диагностики |
| **Bluetooth** | Встроенный в ESP32 | SPP (Serial Port Profile) | Подключение к Android |

---

## Архитектура

Система построена на **детерминированной шине сообщений (Pub/Sub)**, а не на прямых вызовах функций или глобальных переменных. Это фундаментальное архитектурное решение.

### DataRouter — шина данных

| Принцип | Описание |
|---------|----------|
| **Анонимность отправителя** | Модуль подписывается на `TOPIC_ENGINE_PACK`, а не на `SimulatorTask`. Отправитель может быть заменён (`REAL_ENGINE_ENABLED`) без правок в приёмниках. |
| **Типизированные топики** | Каждый топик имеет строго определённый тип данных: `EnginePack`, `TripPack`, `CmdPayload`, `char[]`, `bool`. Нет единого `BusMessage`. |
| **Очередь создаётся модулем** | Подписчик сам создаёт `xQueueCreate()`, затем регистрирует через `DataRouter.subscribe()`. Маршрутизатор только управляет dispatch. |
| **Атомарность состояния** | Связанные параметры сгруппированы в **5 агрегированных пакетов** (`EnginePack`, `TripPack`, `KlinePack`, `ClimatePack`, `SettingsPack`). Данные не конфликтуют, изменение пробега/расхода/топлива происходит синхронно. |
| **Нулевые блокировки в publish()** | `publish()` копирует данные в очереди подписчиков и возвращается за **3–5 мкс**. Никакой сериализации или бизнес-логики внутри шины. |
| **Retained State Cache** | Шина хранит последнее значение каждого топика. При подписке с `retain=true` модуль мгновенно получает актуальное состояние, даже если отправитель опубликовал его раньше. |

### Диаграмма компонентов

```
┌─────────────────────────────────────────────────────────────────┐
│               DataRouter v6.0 (Singleton)                       │
│  Типизированные топики, модуль создаёт очередь → subscribe()    │
│         publish() → memcpy в очереди подписчиков                │
└──────┬──────────┬───────────┬──────────┬────────────┬───────────┘
       │          │           │          │            │
┌──────▼──┐  ┌───▼────┐  ┌──▼─────┐  ┌─▼────────┐ ┌▼──────────┐
│Simulator│  │Calc-   │  │Proto-  │  │ Storage  │ │  BT Trans │
│  Task   │  │ ulator │  │  col   │  │   Task   │ │SPP ↔ Router│
└──────┬──┘  └───┬────┘  └──┬─────┘  └─┬────────┘ └───────────┘
       │         │           │           │
┌──────▼──┐ ┌───▼────┐ ┌───▼─────┐
│  K-Line │ │Climate │ │  OLED   │
│ Task    │ │  Task  │ │  Task   │
└─────────┘ └────────┘ └─────────┘
```

### Поток данных

#### EnginePack → Calculator → TripPack → Storage
```
[Simulator] ──publishPacket(EnginePack)──► TOPIC_ENGINE_PACK
                                              │
                                    Calculator.subscribePacket()
                                              │
                              Расчёт: odo=base+distance, trip=base+distance
                                              │
                              [Calculator] ──publishPacket(TripPack)──► TOPIC_TRIP_PACK
                                                                            │
                                                                  Storage.subscribePacket()
                                                                            │
                                                                  putBytes("trip", pack)
```

#### Фракционная телеметрия (Android)
```
EnginePack ──► Protocol кэш ──┐
TripPack   ──► Protocol кэш ──┼── FAST (100мс) ──► TOPIC_MSG_OUTGOING ──► BT ──► Android
ServicePack──► Protocol кэш ──┤── TRIP (500мс) ──► TOPIC_MSG_OUTGOING
                              └── SERVICE (1000мс)──► TOPIC_MSG_OUTGOING
```

#### Путь команд (Android → Calculator/K-Line)
```
Android ──► SerialBT ──► BT Transport ──publish──► TOPIC_MSG_INCOMING
                                                          │
                                                 Protocol.handleCommand()
                                                          │
                                        ┌─────────────────┼─────────────────┐
                                        ▼                 ▼                 ▼
                                  TOPIC_CMD        TOPIC_CMD        TOPIC_MSG_OUTGOING
                                        │                 │            (ack_id)
                              Calculator.onCmd()   KLine.onCmd()
```

---

## Модули системы

| Модуль | Файл | Приоритет | Роль |
|--------|------|-----------|------|
| **DataRouter** | `data_router.h/cpp` | — | Центральная шина Pub/Sub с типизированными топиками |
| **Simulator** | `simulator_task.cpp` | 2 | Виртуальный двигатель: физика автомобиля, обработка кнопки/педали |
| **Calculator** | `calculator.h/cpp` | 2 | Обогащатель данных: расчёт ODO, Trip, Fuel, Avg по формуле `current = base + accumulated` |
| **Protocol** | `protocol_task.cpp` | 3 | JSON-протокол: парсинг команд Android, фракционная сборка телеметрии |
| **Storage** | `storage_task.cpp` | 1 | NVS-хранилище: сохранение/загрузка TripPack и SettingsPack |
| **BT Transport** | `bt_transport.cpp` | — | Bluetooth SPP: «труба» SerialBT ↔ DataRouter |
| **K-Line** | `kline_task.cpp` | 1 | Диагностика ЭБУ: опрос по K-Line (ISO 9141-2), автоопределение протокола |
| **Climate** | `climate.cpp` | 1 | Климат/сервис: температуры салона/улицы, шины, омывайка |
| **OLED** | `oled_task.cpp` | 1 | Дисплей SSD1306 128×64 (I2C): визуализация телеметрии |

### Пакеты данных

| Пакет | Размер | Топик | Период | Описание |
|-------|--------|-------|--------|----------|
| **EnginePack** | 26 байт | `TOPIC_ENGINE_PACK` | 100 мс | Скорость, RPM, напряжение, статус двигателя, передача, топливо |
| **TripPack** | 41 байт | `TOPIC_TRIP_PACK` | 1000 мс | Одометр, Trip A/B, расход топлива, средний расход |
| **KlinePack** | 74 байта | `TOPIC_KLINE_PACK` | 1000 мс | Температуры ЭБУ, коды ошибок (DTC) |
| **ClimatePack** | 11 байт | `TOPIC_CLIMATE_PACK` | 1000 мс | Температура салона/улицы, шины, омывайка |
| **SettingsPack** | 19 байт | `TOPIC_SETTINGS_PACK` | При изменении | Ёмкость бака, форсунки, датчик скорости, протокол K-Line |

### Команды (13 шт)

| Команда | Код | Обработчик | Описание |
|---------|-----|------------|----------|
| `CMD_RESET_TRIP_A` | 0x01 | Calculator | Сброс Trip A и расхода A |
| `CMD_RESET_TRIP_B` | 0x02 | Calculator | Сброс Trip B и расхода B |
| `CMD_RESET_AVG` | 0x03 | Calculator | Сброс среднего расхода |
| `CMD_FULL_TANK` | 0x04 | Calculator | Заправка (полный бак = tank_capacity) |
| `CMD_CORRECT_ODO` | 0x05 | Calculator | Корректировка одометра |
| `CMD_GET_CFG` | 0x06 | Protocol | Запрос настроек |
| `CMD_SET_CFG` | 0x07 | Protocol | Установка настроек |
| `CMD_KL_GET_DTC` | 0x08 | K-Line | Чтение кодов ошибок ЭБУ |
| `CMD_KL_CLEAR_DTC` | 0x09 | K-Line | Сброс кодов ошибок ЭБУ |
| `CMD_KL_RESET_ADAPT` | 0x0A | K-Line | Сброс адаптации АКПП |
| `CMD_KL_PUMP_ATF` | 0x0B | K-Line | Прокачка жидкости АКПП |
| `CMD_KL_DETECT_PROTO` | 0x0C | K-Line | Автоопределение протокола K-Line |

---

## Протокол связи с Android

Связь между ESP32 и Android-приложением осуществляется через **Bluetooth SPP** (Serial Port Profile) в формате JSON.

### Формат команд (Android → ESP32)

```json
{"command": "reset_trip_a", "msg_id": 1}
{"command": "full_tank", "msg_id": 2}
{"command": "set_cfg", "msg_id": 3, "tank": 65.0, "inj_cnt": 4}
{"command": "kl_get_dtc", "msg_id": 4}
```

### Формат телеметрии (ESP32 → Android)

Телеметрия отправляется **фракционно** (ТЗ п.5.1):

| Слой | Период | Содержимое |
|------|--------|------------|
| **FAST** | 100 мс | Скорость, RPM, напряжение, двигатель, передача, мгновенный расход |
| **TRIP** | 500 мс | FAST + ODO, Trip A/B, расход топлива, уровень топлива, средний расход |
| **SERVICE** | 1000 мс | FAST + TRIP + температуры ЭБУ, DTC, климат, шины, омывайка |

```json
{
  "spd": 65,
  "rpm": 2500,
  "vlt": 14.2,
  "eng": true,
  "gear": 3,
  "inst": 8.5,
  "odo": 152345.6,
  "trip_a": 123.4,
  "fuel_a": 12.3,
  "avg_cur": 9.8,
  "fuel": 35.2,
  "t_cool": 87.5,
  "t_atf": 75.0,
  "t_int": 22.0,
  "t_ext": -5.0
}
```

### Ответ-подтверждение (ack_id)

Каждая команда получает подтверждение:

```json
{"ack_id": 1, "status": "ok"}
```

---

## Установка и сборка

### Предварительные требования

- **VS Code** с расширением [PlatformIO IDE](https://platformio.org/install/ide?install=vscode)
- **Git** для клонирования репозитория
- **ESP32 WEMOS D1 MINI32** (или совместимая плата)
- **USB-кабель** для прошивки

### Шаги

1. **Клонируйте репозиторий:**
   ```bash
   git clone https://github.com/AlexeyBar3073/Bluetooth_connect_v2.git
   cd Bluetooth_connect_v2
   ```

2. **Установите зависимости PlatformIO:**
   ```bash
   pio lib install
   ```
   Автоматически установятся:
   - [ArduinoJson](https://github.com/bblanchon/ArduinoJson) — JSON-сериализация
   - [U8g2](https://github.com/olikraus/u8g2) — библиотека OLED дисплея

3. **Подключите ESP32** к компьютеру через USB

4. **Соберите и прошейте:**
   ```bash
   pio run --target upload
   ```

5. **Откройте монитор порта:**
   ```bash
   pio device monitor
   ```

### Конфигурация платы

Настройки PlatformIO указаны в `platformio.ini`:

```ini
[env:wemos_d1_mini32]
platform = espressif32
board = wemos_d1_mini32
framework = arduino
upload_speed = 921600
monitor_speed = 115200
```

---

## Конфигурация

Основные параметры настраиваются в `include/app_config.h`:

### Версия прошивки
```c
#define FW_VERSION_MAJOR 6
#define FW_VERSION_MINOR 0
#define FW_VERSION_BUILD 0
#define FW_VERSION_STR   "6.0.0"
```

### Аппаратные пины
```c
#define POTENTIOMETER_PIN   33   // Потенциометр «газа» (ADC)
#define BUTTON_PIN          26   // Кнопка запуска двигателя
```

### Параметры симулятора
```c
#define SPEED_MAX           220.0f   // Максимальная скорость (км/ч)
#define RPM_IDLE            750.0f   // Обороты холостого хода
#define RPM_MAX             6500.0f  // Максимальные обороты
#define ACCELERATION_TIME   10.0f    // Время разгона 0-100 км/ч (сек)
```

### Приоритеты FreeRTOS
```c
#define TASK_PRIORITY_SIMULATOR  2   // Физика автомобиля
#define TASK_PRIORITY_CALCULATOR 2   // Расчёт TripPack
#define TASK_PRIORITY_PROTOCOL   3   // JSON-протокол (высший приоритет)
#define TASK_PRIORITY_OLED       1   // OLED дисплей
#define TASK_PRIORITY_STORAGE    1   // NVS-хранилище
#define TASK_PRIORITY_KLINE      1   // K-Line диагностика
#define TASK_PRIORITY_CLIMATE    1   // Климат/сервис
```

---

## Версионирование

Проект использует формат `MAJOR.MINOR.BUILD`:

| Изменение | Что менять | Пример |
|-----------|-----------|--------|
| **Кардинальное изменение** архитектуры | `MAJOR`, MINOR=0, BUILD=0 | DataRouter → `6.0.0` |
| **Новый модуль/участник** в системе | `MINOR` +1, BUILD=0 | Добавлен WiFi → `6.1.0` |
| **Новая функциональность** (команда, экран) | `MINOR` +1, BUILD=0 | Команда kl_pump_atf → `6.1.0` |
| **Исправление ошибок**, рефакторинг без смены логики | `BUILD` +1 | Фикс расчёта TripPack → `6.0.1` |
| **Поправка в тексте** (комменты, логи) | Ничего | — |

### Лог изменений

| Версия | Дата | Описание |
|--------|------|----------|
| **6.0.0** | 2026-04-09 | MAJOR: DataRouter — типизированные топики, очереди у модулей, без BusMessage |
| **5.1.0** | 2026-04-08 | Разделение ServicePack → KlinePack + ClimatePack |
| **5.0.0** | 2026-04-07 | MAJOR: Переход на агрегированные пакеты. 47 топиков → 9. NVS-хранение пакетов. Loop-диспетчер с heartbeat. |
| **4.2.1** | 2026-04-05 | Фикс: ACK msg_id тип int, fuel_a/fuel_b накопление |
| **4.2.0** | 2026-04-05 | Фикс протокола, Tank/Fuel синхронизация, K-Line dummy DTCs |
| **4.0.0** | 2026-04-05 | MAJOR: K-Line диагностика, Incremental Telemetry |
| **3.0.0** | 2026-04-04 | MAJOR: Async DataBus, разделение Simulator↔VehicleModel |

Полный лог — в [AGENTS.md](AGENTS.md).

---

## Структура проекта

```
Bluetooth_connect_v2/
├── include/                  # Заголовочные файлы
│   ├── app_config.h          # Конфигурация прошивки (версия, пины, приоритеты)
│   ├── calculator.h          # Calculator — расчётное ядро
│   ├── bt_transport.h        # Bluetooth SPP транспорт
│   ├── data_router.h         # DataRouter — шина Pub/Sub
│   ├── topics.h              # Реестр топиков шины
│   ├── protocol_task.h       # Protocol Task — JSON-протокол
│   ├── storage_task.h        # Storage Task — NVS-хранилище
│   ├── simulator_task.h      # Simulator Task — виртуальный двигатель
│   ├── oled_task.h           # OLED Task — дисплей
│   ├── kline_task.h          # K-Line Task — диагностика ЭБУ
│   ├── climate.h             # Climate Task — климат/сервис
│   └── icons.h               # Иконки для OLED дисплея
│
├── src/
│   ├── main.cpp              # Точка входа, инициализация, Loop-диспетчер
│   ├── calculator.cpp        # Расчёт TripPack, обработка команд
│   ├── data_router.cpp       # Реализация DataRouter (Pub/Sub)
│   ├── bt_transport.cpp      # Bluetooth SPP (SerialBT ↔ DataRouter)
│   ├── protocol_task.cpp     # Парсинг JSON, фракционная телеметрия
│   ├── storage_task.cpp      # NVS сохранение/загрузка пакетов
│   ├── simulator_task.cpp    # Физика автомобиля, ввод водителя
│   ├── oled_task.cpp         # Отрисовка на SSD1306
│   ├── kline_task.cpp        # Опрос ЭБУ по K-Line (или симуляция)
│   ├── climate.cpp           # Датчики климата (или симуляция)
│   └── common/
│       ├── packets.h         # Агрегированные пакеты (EnginePack, TripPack...)
│       └── commands.h        # Реестр команд (13 команд, CmdPayload)
│
├── test/                     # Модульные тесты
│   └── test_data_bus.cpp     # Тесты шины данных
│
├── docs/                     # Документация
│   ├── PARADIGMA.md          # Архитектура шины данных (Pub/Sub парадигма)
│   ├── KLINE_RESEARCH.md     # Исследование K-Line протоколов
│   └── PROTOCOL_PARADIGM.md  # Протокол связи с Android
│
├── .gitignore                # Git ignore rules
├── AGENTS.md                 # Правила работы с проектом (для AI-ассистентов)
└── platformio.ini            # PlatformIO конфигурация проекта
```

---

## Документация

| Файл | Описание |
|------|----------|
| [AGENTS.md](AGENTS.md) | Правила работы с проектом: архитектура, модули, поток данных, правила кода, лог изменений |
| [docs/PARADIGMA.md](docs/PARADIGMA.md) | Архитектура шины данных: парадигма Pub/Sub, политики очередей, RAM-математика, чек-лист |
| [docs/KLINE_RESEARCH.md](docs/KLINE_RESEARCH.md) | Исследование K-Line протоколов: ISO 9141-2, KWP2000, ISO 14230 |
| [docs/PROTOCOL_PARADIGM.md](docs/PROTOCOL_PARADIGM.md) | Протокол связи с Android: формат JSON-команд, фракционная телеметрия |

---

## Лицензия

MIT

---

## Авторы

- **AlexeyBar3073** — [GitHub](https://github.com/AlexeyBar3073)
