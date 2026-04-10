// -----------------------------------------------------------------------------
// kline_task.h
// K-Line диагностика Toyota через MC33290 (ISO 9141-2).
//
// Назначение:
// - Подключение к ЭБУ Toyota RAV4 2004 по протоколу ISO 9141-2 (K-Line)
// - Чтение параметров: температура ОЖ/ATF, коды ошибок (DTC)
// - Чтение и сброс кодов ошибок из всех блоков (ECM, TCM, ABS, SRS)
// - Сброс адаптации АКПП и прокачка ABS (через Mode 31 Active Test)
//
// Подключение MC33290:
// - Pin 1 (TXD) → GPIO 16 (UART2 RX)
// - Pin 2 (RXD) → GPIO 17 (UART2 TX)
// - Pin 5 (K-Line) → OBD-II Pin 7 (через резистор 510Ω к +12V)
//
// Протокол:
// - Инициализация: 5-baud slow init (ISO 9141-2)
// - Скорость: 10400 baud, 8N1
// - Адреса ECU: ECM=0x7E0, TCM=0x7E1, ABS=0x7E3
//
// Архитектура v6.1.0: DataRouter, типизированные топики, module-owned queues
// -----------------------------------------------------------------------------

#ifndef KLINE_TASK_H
#define KLINE_TASK_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// --- Пины UART2 для MC33290 ---
#define KLINE_UART_TX_PIN 17     // UART2 TX → MC33290 RXD (pin 2)
#define KLINE_UART_RX_PIN 16     // UART2 RX ← MC33290 TXD (pin 1)
#define KLINE_UART_NUM  2        // Используется UART2 (UART0 — Serial для отладки)

// --- Таймауты ISO 9141-2 ---
#define KLINE_INIT_WAIT_MS    300    // Ожидание после включения зажигания перед инициализацией
#define KLINE_SLOW_INIT_BIT_MS 200   // Длительность одного бита при 5 baud (1000/5 = 200мс)
#define KLINE_W1_TIMEOUT_MS    25    // Максимальное ожидание ответа от ECU после адреса
#define KLINE_UART_BAUD     10400    // Скорость UART после успешной инициализации

// --- Адреса блоков управления (ECU) ---
#define KLINE_ECU_ECM  0x7E0    // Engine Control Module — двигатель, впрыск, зажигание
#define KLINE_ECU_TCM  0x7E1    // Transmission Control Module — АКПП
#define KLINE_ECU_ABS  0x7E3    // ABS Control Module — антиблокировочная система

// --- Размеры буферов ---
#define KLINE_RX_BUFFER_SIZE 128  // Максимальный размер буфера приёма
#define KLINE_TX_BUFFER_SIZE 64   // Максимальный размер буфера передачи

// =============================================================================
// Публичный API
// =============================================================================

// klineStart: Запускает FreeRTOS задачу K-Line диагностики.
// Выполняет инициализацию UART2, ISO 9141-2 slow init, подписку на команды.
// После запуска задача периодически опрашивает ECM и публикует KlinePack в TOPIC_KLINE_PACK.
void klineStart();

// klineStop: Останавливает задачу K-Line, освобождает ресурсы UART.
void klineStop();

// klineIsRunning: Возвращает true если задача K-Line активна.
bool klineIsRunning();

// klineIsConnected: Возвращает true если установлена связь с ECU.
// Обновляется после каждой попытки инициализации.
bool klineIsConnected();

#endif // KLINE_TASK_H
