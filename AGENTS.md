# AGENTS.md — Правила работы с проектом Bluetooth_connect_v2

> **Последнее обновление:** 2026-04-10
> **Текущая версия:** 6.2.1 (build 1)
> **Заметка:** FIX: калибровка датчика скорости через Android

---

## 📋 О ПРОЕКТЕ

**Bluetooth_connect_v2** — Бортовой компьютер автомобиля на ESP32 с модульной Pub/Sub архитектурой (шина данных на основе FreeRTOS Queue), связью по Bluetooth с Android-приложением и энергонезависимой памятью (NVS).

**Платформа:** ESP32 WEMOS D1 MINI32 (Espressif32, Arduino framework, FreeRTOS)

**Архитектура v6.1.0:** DataRouter — типизированные топики, очереди принадлежат модулям, нет единого BusMessage. Все модули (Simulator, Calculator, Protocol, Storage, OLED, K-Line, Climate) подключены и работают через шину.

---

## 🏷️ ПРАВИЛА ВЕРСИОНИРОВАНИЯ

Формат: `MAJOR.MINOR.BUILD`

| Изменение | Что менять | Пример |
|-----------|-----------|--------|
| **Кардинальное изменение** архитектуры | `MAJOR`, MINOR=0, BUILD=0 | DataRouter → `6.0.0` |
| **Новый модуль/участник** в системе | `MINOR` +1, BUILD=0 | Добавлен WiFi → `6.1.0` |
| **Новая функциональность** (команда, экран) | `MINOR` +1, BUILD=0 | Команда kl_pump_atf → `6.1.0` |
| **Исправление ошибок**, рефакторинг без смены логики | `BUILD` +1 | Фикс расчёта TripPack → `6.0.1` |
| **Поправка в тексте** (комменты, логи) | Ничего | — |

**Где менять:** `include/app_config.h` — `FW_VERSION_MAJOR`, `FW_VERSION_MINOR`, `FW_VERSION_BUILD`, `FW_VERSION_STR`, `FW_VERSION_NOTE`

---

## 🏗️ АРХИТЕКТУРА (v6.1.0 — DataRouter, все модули подключены)

```
┌─────────────────────────────────────────────────────────────────┐
│               DataRouter v6.0 (Singleton)                       │
│  Типизированные топики, модуль создаёт очередь → subscribe()    │
│         publish() → memcpy в очереди подписчиков                │
└──────┬──────────┬───────────┬──────────┬────────────┬───────────┘
       │          │           │          │            │
┌──────▼──┐  ┌───▼────┐  ┌──▼─────┐  ┌─▼────────┐ ┌▼──────────┐
│Simulator│  │Calc-   │  │Proto-  │  │ Storage  │ │  BT Trans │
│  Task   │  │ ulator │  │  col   │  │   Task   │ │   port    │
│EnginePack│ │TripPack│  │  Task  │  │  (NVS)   │ │SPP ↔ Router│
└──────┬──┘  └───┬────┘  └──┬─────┘  └─┬────────┘ └───────────┘
       │         │           │           │
┌──────▼──┐ ┌───▼────┐ ┌───▼─────┐
│  K-Line │ │Climate │ │  OLED   │
│ Task    │ │  Task  │ │  Task   │
│KlinePack│ │ClimateP│ │Display  │
└─────────┘ └────────┘ └─────────┘
```

### Ключевые принципы

1. **Модули НЕ знают друг о друге** — общение ТОЛЬКО через DataRouter.
2. **Очередь создаётся МОДУЛЕМ** — `xQueueCreate()` у себя, затем `DataRouter.subscribe()`.
3. **Типизированные топики** — каждый топик имеет свой тип данных:
   - `TOPIC_CMD` → `uint8_t` (enum Command, 1 байт)
   - `TOPIC_ENGINE_PACK` → `EnginePack` (27 байт)
   - `TOPIC_MSG_INCOMING` → `char[256]` (JSON-строка)
   - `TOPIC_TRANSPORT_STATUS` → `bool` (1 байт)
4. **Simulator — виртуальный двигатель:**
   - Кнопка (GPIO26) и педаль (GPIO33) обрабатываются ВНУТРИ СЕБЯ
   - Физика (RPM, Speed, Fuel) считается ВНУТРИ СЕБЯ
   - Публикует EnginePack каждые 100 мс
5. **Calculator — обогащатель данных:**
   - Подписывается на EnginePack (distance, fuel_used, eng)
   - Считает ODO, Trip, Avg по формуле: `current = base + accumulated`
   - Публикует TripPack каждые 1000 мс
6. **Нет String в шине** — используем `char[]` / `const char*`.
7. **publish() возвращает bool** — проверка успешности доставки.

---

## 📦 МОДУЛИ

### DataRouter (data_router.h/cpp)
- **Роль:** Центральная асинхронная шина (Pub/Sub) с типизированными топиками
- **Механизм:** Модуль создаёт очередь → `subscribe()` → `publish()` копирует в очереди
- **Типы данных:** float, bool, int, double, uint8_t (команды), char[] (строки), бинарные пакеты
- **Методы publish:** `publish(topic, value)`, `publishPacket(topic, data, len)`, `publishString(topic, str)`
- **Кэш:** Хранит последние значения для `bool`, `Command`
- **Нельзя:** вызывать callback напрямую из publish, использовать String, блокировать publish()

### Topics (topics.h)
- **Роль:** Реестр тем шины данных (9 топиков)
- **Пакетные топики (0x01–0x0F):**
  - `TOPIC_ENGINE_PACK` (0x01) — EnginePack (FAST, 100 мс)
  - `TOPIC_TRIP_PACK` (0x02) — TripPack (TRIP, 1 сек)
  - `TOPIC_SERVICE_PACK` (0x03) — ServicePack (SERVICE, 1 сек)
  - `TOPIC_SETTINGS_PACK` (0x04) — SettingsPack (настройки, retain)
- **Управляющие топики (0xF0–0xFF):**
  - `TOPIC_CMD` (0xF0) — команды (uint8_t, enum Command)
  - `TOPIC_MSG_INCOMING` (0xF1) — JSON от Android
  - `TOPIC_MSG_OUTGOING` (0xF2) — JSON на Android
  - `TOPIC_TRANSPORT_STATUS` (0xF3) — статус Bluetooth
  - `TOPIC_NOT_FUEL` (0xF4) — датчик топлива отсутствует
- **Нельзя:** менять числовые значения топиков, удалять топики

### Packets (common/packets.h)
- **Роль:** Структуры агрегированных пакетов (#pragma pack(1))
- **EnginePack (34 байта, v2):** speed, rpm, voltage, engine_running, parking_lights, instant_fuel, distance, fuel_used, gear, selector_pos[4], tcc_lockup
- **TripPack (41 байт, v1):** odo, trip_a, fuel_trip_a, trip_b, fuel_trip_b, trip_cur, fuel_cur, fuel_level, avg_consumption
- **ServicePack (84 байта, v1):** coolant_temp, atf_temp, dtc_count, dtc_codes[64], interior_temp, exterior_temp, tire_pressure, washer_level
- **SettingsPack (19 байт, v1):** tank_capacity, injector_count, injector_flow, pulses_per_meter, kline_protocol
- **Нельзя:** менять порядок полей, удалять поля, убирать #pragma pack

### Commands (common/commands.h)
- **Роль:** Типобезопасные команды (13 шт)
- **Enum Command:** CMD_NONE, CMD_RESET_TRIP_A, CMD_RESET_TRIP_B, CMD_RESET_AVG, CMD_FULL_TANK, CMD_CORRECT_ODO, CMD_GET_CFG, CMD_SET_CFG, CMD_KL_GET_DTC, CMD_KL_CLEAR_DTC, CMD_KL_RESET_ADAPT, CMD_KL_PUMP_ATF, CMD_KL_DETECT_PROTO
- **Типизированные пакеты:** EnginePack, TripPack, ServicePack, SettingsPack, KlinePack, ClimatePack
- **Команды:** uint8_t (enum Command)
- **Сообщения:** char[] (JSON строки)
- **Нельзя:** менять значения enum, удалять команды

### Simulator (simulator_task.cpp) — ВИРТУАЛЬНЫЙ ДВИГАТЕЛЬ
- **Роль:** Эмуляция физики автомобиля и обработка ввода водителя
- **Аппаратные элементы:** Кнопка GPIO26 (долгое нажатие ≥800мс), потенциометр GPIO33 (ADC 12 бит)
- **Публикует:** EnginePack каждые 100 мс в TOPIC_ENGINE_PACK
- **Подписан на:** TOPIC_CMD (full_tank), TOPIC_SETTINGS_PACK (настройки), TOPIC_NOT_FUEL
- **Физика:** инерция скорости (0-100 за 10с), RPM (кусочно-линейная), расход (5 + speed/100*10), напряжение (12.7/13.5-14.5В)
- **Нельзя:** вызывать Calculator/VehicleModel напрямую, публиковать чаще 100 мс

### Calculator (calculator.h/cpp) — ОБОГАЩАТЕЛЬ ДАННЫХ
- **Роль:** Расчёт производных параметров на основе EnginePack
- **Подписан на:** TOPIC_ENGINE_PACK, TOPIC_TRIP_PACK (base), TOPIC_SETTINGS_PACK, TOPIC_NOT_FUEL, TOPIC_CMD
- **Публикует:** TripPack каждые 1000 мс в TOPIC_TRIP_PACK
- **Формулы:** odo = base + distance, trip = base + distance, avg = fuel_used / distance * 100
- **Защита от перезапуска Storage:** odo > odo_current → обновить base
- **Нельзя:** генерировать скорость/RPM самостоятельно, писать в NVS напрямую

### Storage (storage_task.cpp) — NVS ХРАНИЛИЩЕ
- **Роль:** Бинарное хранение TripPack и SettingsPack в NVS (putBytes/getBytes)
- **При старте:** загружает пакеты из NVS, публикует в DataBus
- **В работе:** подписан на TripPack и SettingsPack → сохраняет при изменении (ограничение 100 мс)
- **NVS-ключи:** "trip" (TripPack), "settings" (SettingsPack)
- **Нельзя:** писать в NVS чаще 100 мс, вызывать модули напрямую

### Protocol (protocol_task.cpp) — JSON-ПРОТОКОЛ
- **Роль:** Парсинг команд Android, фракционная сборка JSON телеметрии
- **Подписан на:** EnginePack, TripPack, ServicePack, SettingsPack (обновление кэша), TOPIC_MSG_INCOMING (команды), TOPIC_TRANSPORT_STATUS
- **Публикует:** TOPIC_MSG_OUTGOING (FAST 100мс / TRIP 500мс / SERVICE 1000мс), TOPIC_CMD
- **13 команд:** reset_trip_a/b, reset_avg, full_tank, correct_odo, get_cfg, set_cfg, kl_get_dtc, kl_clear_dtc, kl_reset_adapt, kl_pump_atf, kl_detect_protocol
- **Нельзя:** блокировать задачу >10 мс, использовать String для JSON

### BT Transport (bt_transport.cpp) — BLUETOOTH SPP
- **Роль:** «Труба» SerialBT ↔ DataBus
- **RX:** SerialBT → TOPIC_MSG_INCOMING
- **TX:** TOPIC_MSG_OUTGOING → SerialBT
- **Статус:** TOPIC_TRANSPORT_STATUS при изменении
- **Нельзя:** парсить JSON, блокировать >10 мс

### K-Line (kline_task.cpp) — ДИАГНОСТИКА ЭБУ
- **Роль:** Опрос ЭБУ по К-Line (симуляция/реальный), автоопределение протокола
- **Публикует:** KlinePack каждые 1000 мс в TOPIC_KLINE_PACK (температуры, DTC)
- **Подписан на:** TOPIC_CMD (kl_* команды, QueuePolicy::FIFO_DROP, depth=5)
- **Очередь:** модуль создаёт `xQueueCreate(5, sizeof(uint8_t))` сам
- **Режим симуляции:** тестовые данные (coolant=85-95°C, atf=70-85°C, DTC="P0135;P0141")
- **Нельзя:** публиковать KlinePack чаще 1000 мс, формировать JSON, использовать DataBus

### Climate (climate.cpp) — КЛИМАТ/СЕРВИС
- **Роль:** Чтение сервисных датчиков (или симуляция)
- **Публикует:** ClimatePack каждые 1000 мс в TOPIC_CLIMATE_PACK (interior_temp, exterior_temp, tire_pressure, washer_level)
- **Режим симуляции:** случайные данные с низким вероятностью предупреждений
- **Нельзя:** блокировать >100 мс, публиковать в KlinePack

### OLED (oled_task.cpp) — ДИСПЛЕЙ
- **Роль:** Визуализация на SSD1306 128x64 (I2C, SDA=21, SCL=22)
- **Подписан на:** EnginePack, TripPack, SettingsPack, TOPIC_TRANSPORT_STATUS
- **Частота:** 5 Гц (каждые 200 мс)
- **Layout:** BT статус, ENG статус, скорость, RPM, топливо + прогресс-бар
- **Нельзя:** вызывать sendBuffer() внутри callback

---

## 🔄 ПОТОК ДАННЫХ

### EnginePack → Calculator → TripPack → Storage
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

### Фракционная телеметрия (Android)
```
EnginePack ──► Protocol кэш ──┐
TripPack   ──► Protocol кэш ──┼── FAST (100мс) ──► TOPIC_MSG_OUTGOING ──► BT ──► Android
ServicePack──► Protocol кэш ──┤── TRIP (500мс) ──► TOPIC_MSG_OUTGOING
                              └── SERVICE (1000мс)──► TOPIC_MSG_OUTGOING
```

### Путь команд (Android → Calculator/K-Line)
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

## 🔧 ПРАВИЛА КОДА

### ✅ Можно
- Добавлять новые поля в КОНЕЦ структур пакетов (обратная совместимость)
- Добавлять новые команды в КОНЕЦ enum Command
- Добавлять новые топики в КОНЕЦ enum Topic (перед TOPIC_COUNT)
- Менять формулы в Calculator и Simulator
- Добавлять реальные датчики в Climate и K-Line
- Подписывать модули на новые топики/пакеты

### ❌ Нельзя
- Менять порядок полей в пакетах (#pragma pack(1) — бинарная совместимость)
- Менять числовые значения топиков или команд
- Вызывать модули напрямую (только через DataBus)
- Использовать String в публикациях (только char[])
- Блокировать Dispatcher Task или Callback >100 мкс
- Писать в NVS чаще 100 мс
- Забывать обновлять версию в app_config.h при изменении архитектуры

---

## 📝 ЛОГ ИЗМЕНЕНИЙ

| Версия | Дата | Описание |
|--------|------|----------|
| **6.2.1** | 2026-04-10 | FIX: калибровка датчика скорости. Команды `calibrate_speed_start` / `calibrate_speed_end`, расчёт `pulses_per_meter`, сохранение в NVS через SettingsPack. |
| **6.2.0** | 2026-04-10 | **MINOR: RealEngine + INA226.** ISR форсунки (GPIO4, CHANGE) + геркон (GPIO13, RISING). INA226: бортсеть + поплавок-реостат. DEBUG_LOG, OLED_ENABLED, REAL_ENGINE_ENABLED — переключатели через platformio.ini. |
| **6.1.1** | 2026-04-10 | FIX: txBuffer overflow — очередь BT Transport 512B, но локальный буфер был 256B → краш при `get_cfg`. |
| **6.1.0** | 2026-04-10 | **MINOR: K-Line и Climate модули подключены.** K-Line переведён с DataBus на DataRouter (module-owned queue). Оба модуля запущены в main.cpp, heartbeat-мониторинг добавлен. |
| **6.0.0** | 2026-04-09 | MAJOR: DataRouter — типизированные топики, очереди у модулей, без BusMessage. Экономия RAM ~10 КБ. |
| **5.0.0** | 2026-04-07 | **MAJOR: Переход на агрегированные пакеты.** 47 топиков → 9. EnginePack, TripPack, ServicePack, SettingsPack. Бинарное хранение в NVS. Loop-диспетчер с heartbeat. Калькулятор вместо VehicleModel. Новый модуль Climate. |
| **4.2.1** | 2026-04-05 | Фикс: ACK msg_id тип int, fuel_a/fuel_b накопление. |
| **4.2.0** | 2026-04-05 | Фикс протокола, Tank/Fuel синхронизация, K-Line dummy DTCs. |
| **4.1.0** | 2026-04-05 | Рефакторинг: Simulator=физика, VehicleModel=интегратор. DataBus Queue=128. |
| **4.0.0** | 2026-04-05 | MAJOR: K-Line диагностика, Incremental Telemetry. |
| **3.0.0** | 2026-04-04 | MAJOR: Async DataBus, разделение Simulator↔VehicleModel. |
| 2.3 | — | Команды Android, Storage Task, напряжение 12.7V. |
| 2.0 | — | Начальная Pub/Sub архитектура. |

---

## ⚠️ ОБНОВЛЕНИЕ AGENTS.md

**Правило:** При каждом изменении проекта необходимо обновлять этот файл.

### Что обновлять:
1. **Дата и версия** в шапке
2. **Раздел АРХИТЕКТУРА** — при изменении структуры
3. **Раздел МОДУЛИ** — при добавлении/изменении модулей
4. **Раздел ПОТОК ДАННЫХ** — при изменении маршрутов
5. **Раздел ПРАВИЛА КОДА** — при новых ограничениях
6. **Раздел ЛОГ ИЗМЕНЕНИЙ** — каждая значимая правка

---

## 🧠 ЧТО ПОМНИТЬ

1. **DataRouter.begin()** — ПЕРЕД запуском любых задач
2. **Callback <100 мкс** — только присвоение переменных
3. **JSON:** `char buffer[512]` + `serializeJson(doc, buffer)`
4. **Пакеты:** `publishPacket(topic, &pack, sizeof(pack))`
5. **Команды:** `uint8_t cmd; if (xQueueReceive(cmdQ, &cmd, 0) == pdTRUE) { if ((Command)cmd == CMD_...) { ... } }`
6. **NVS:** putBytes/getBytes для пакетов, не чаще 100 мс
7. **Loop:** heartbeat-мониторинг, перезапуск при падении
8. **Модуль создаёт очередь сам** — `xQueueCreate()`, затем `router.subscribe()`
