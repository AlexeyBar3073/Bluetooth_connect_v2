// -----------------------------------------------------------------------------
// commands.h
// Реестр команд для шины DataRouter.
//
// Назначение:
// - Типобезопасная передача команд между модулями через шину (TOPIC_CMD)
// - Единый enum Command (15 команд) — все модули используют одинаковые ID
//
// Поток данных:
//   Android → JSON → Protocol Task → enum Command → TOPIC_CMD → Calculator/K-Line
//
// Полный список команд (15 шт):
//   reset_trip_a    — Сброс Trip A и расхода A
//   reset_trip_b    — Сброс Trip B и расхода B
//   reset_avg       — Сброс среднего расхода
//   full_tank       — Заправка (полный бак = tank_capacity)
//   correct_odo     — Корректировка одометра
//   get_cfg         — Запрос настроек (ответ из кэша Protocol)
//   set_cfg         — Установка настроек (публикация в SettingsPack)
//   kl_get_dtc      — Чтение кодов ошибок ЭБУ
//   kl_clear_dtc    — Сброс кодов ошибок ЭБУ
//   kl_reset_adapt  — Сброс адаптации АКПП
//   kl_pump_atf     — Прокачка АКПП
//   kl_detect_proto — Автоопределение протокола К-Line
//   calibrate_speed   — Калибровка VSS (проехать известное расстояние)
//   calibrate_injector — Калибровка форсунки (заправить полный бак)
//   calibrate_deadtime — Калибровка dead time (работа на холостых)
//
//   ota_end           — Завершение OTA обновления (ESP.restart())
//
// -----------------------------------------------------------------------------
// ПРАВИЛА ФАЙЛА:
// -----------------------------------------------------------------------------
// ✅ Можно:
//   - Добавлять новые команды в КОНЕЦ enum Command
//
// ❌ Нельзя:
//   - Менять числовые значения существующих enum (сломает совместимость)
//   - Удалять существующие команды
//
// ВЕРСИЯ: 5.2.0 — MINOR: Добавлена команда OTA (OTA Task)
// -----------------------------------------------------------------------------

#ifndef COMMANDS_H
#define COMMANDS_H

#include <stdint.h>

// =============================================================================
// Command — Перечисление всех команд (15 шт)
// =============================================================================
//
// Каждая команда соответствует JSON-команде от Android:
//   {"command": "reset_trip_a", "msg_id": 1}
//
// Protocol Task парсит JSON → публикует enum Command в TOPIC_CMD.
// Модули-получатели (Calculator, K-Line) обрабатывают команду.
//
// Диапазоны:
//   0x00        — Нет команды
//   0x01–0x07   — Управление trip/топливом (Calculator)
//   0x08–0x09   — Настройки (Protocol)
//   0x0A–0x0C   — К-Line диагностика (K-Line Task)
//   0x0D–0x0F   — Калибровка (RealEngine Task)
//   0x10        — OTA завершение (OTA Task)
//
enum Command : uint8_t {
    CMD_NONE            = 0x00,  // Нет команды (пустышка, не используется)

    // --- Управление trip/топливом (обрабатывает Calculator Task) ---
    CMD_RESET_TRIP_A    = 0x01,  // Сброс trip_a и fuel_trip_a в 0
    CMD_RESET_TRIP_B    = 0x02,  // Сброс trip_b и fuel_trip_b в 0
    CMD_RESET_AVG       = 0x03,  // Сброс среднего расхода текущей поездки
    CMD_FULL_TANK       = 0x04,  // Заправка: fuel_base = tank_capacity
    CMD_CORRECT_ODO     = 0x05,  // Корректировка одометра (параметр: odo_value)

    // --- Настройки (обрабатывает Protocol Task) ---
    CMD_GET_CFG         = 0x06,  // Запрос всех настроек (ответ из кэша)
    CMD_SET_CFG         = 0x07,  // Установка настроек (параметры в set_cfg)

    // --- К-Line диагностика (обрабатывает K-Line Task) ---
    CMD_KL_GET_DTC      = 0x08,  // Чтение кодов ошибок ЭБУ
    CMD_KL_CLEAR_DTC    = 0x09,  // Сброс кодов ошибок ЭБУ
    CMD_KL_RESET_ADAPT  = 0x0A,  // Сброс адаптации АКПП (TCM)
    CMD_KL_PUMP_ATF     = 0x0B,  // Прокачка жидкости АКПП
    CMD_KL_DETECT_PROTO = 0x0C,   // Принудительное автоопределение протокола

    // --- Калибровка (обрабатывает RealEngine Task) ---
    CMD_CALIBRATE_SPEED     = 0x0D,  // Калибровка VSS (расстояние → pulses_per_meter)
    CMD_CALIBRATE_INJECTOR  = 0x0E,  // Калибровка форсунки (реальный расход → injector_flow)
    CMD_CALIBRATE_DEADTIME  = 0x0F,  // Калибровка dead time (расход на холостых → dead_time_us)

    // --- OTA завершение (обрабатывает OTA Task) ---
    CMD_OTA_END             = 0x10,  // Завершение OTA: Update.end() → ESP.restart()
};

#endif // COMMANDS_H
