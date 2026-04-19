// -----------------------------------------------------------------------------
// main.cpp
// Точка входа и Loop-диспетчер процессов.
//
// Архитектура:
//   SETUP: запуск всех модулей
//   LOOP:  мониторинг задач, перезапуск при падении
//
// ВЕРСИЯ: Определяется в app_config.h (FW_VERSION_STR)
// -----------------------------------------------------------------------------

#include <Arduino.h>
#include "data_router.h"
#include "topics.h"
#include "app_config.h"
#include "bt_transport.h"
#include "debug.h"

// =============================================================================
// Внешние декларации модулей
// =============================================================================

extern void simulatorStart();
extern void simulatorStop();
extern bool simulatorIsRunning();

extern void storageStart();
extern void storageStop();
extern bool storageIsRunning();

extern void calculatorStart();
extern void calculatorStop();
extern bool calculatorIsRunning();

extern void protocolStart();
extern void protocolStop();
extern bool protocolIsRunning();

extern void klineStart();
extern void klineStop();
extern bool klineIsRunning();

extern void climateStart();
extern void climateStop();
extern bool climateIsRunning();

extern void realEngineStart();
extern void realEngineStop();
extern bool realEngineIsRunning();

#if OLED_ENABLED
extern void oledStart();
extern void oledStop();
extern bool oledIsRunning();
#endif

extern bool otaIsInProgress();

// =============================================================================
// Функции перезапуска
// =============================================================================

static void restartSimulator()   { simulatorStop();  delay(100); simulatorStart(); }
static void restartStorage()     { storageStop();    delay(100); storageStart(); }
static void restartCalculator()  { calculatorStop(); delay(100); calculatorStart(); }
static void restartProtocol()    { protocolStop();   delay(100); protocolStart(); }
static void restartKline()       { klineStop();      delay(100); klineStart(); }
static void restartClimate()     { climateStop();    delay(100); climateStart(); }
static void restartRealEngine()  { realEngineStop(); delay(100); realEngineStart(); }
#if OLED_ENABLED
static void restartDisplay()     { oledStop();       delay(100); oledStart(); }
#endif

// =============================================================================
// setup() — запуск всех модулей
// =============================================================================

void setup() {
    // 1. Serial для отладки
    Serial.begin(115200);
    delay(2000);
    
   // 2. Приветствие — с форматированными макросами
    DBG_NEWLINE();
    DBG_PRINTLN("=== Car BKV2 Starting ===");
    DBG_PRINTF("Firmware: v%s", FW_VERSION_STR);
    DBG_PRINTF("Note: %s", FW_VERSION_NOTE);
    DBG_PRINTLN("Architecture: DataRouter");
    DBG_NEWLINE();
    
    // 3. Диагностика — с форматированными макросами
    DBG_PRINTLN("=== System Diagnostics ===");
    DBG_PRINTF("CPU Frequency: %d MHz", ESP.getCpuFreqMHz());
    DBG_PRINTF("Free Heap: %u bytes", ESP.getFreeHeap());
    DBG_PRINTF("Max Alloc Heap: %u bytes", ESP.getMaxAllocHeap());
    DBG_PRINTLN("===========================");
    DBG_NEWLINE();

    // 4. DataRouter — первым!
    DBG_PRINTLN("[SETUP] DataRouter...");
    DataRouter::getInstance().begin();
    delay(50);
    
    // 5. BT Transport — критически важен, запускаем вторым
    DBG_PRINTLN("[SETUP] BT Transport...");
    btTransportStart("Car Simulator");
    delay(200);  // Даём Bluetooth-стеку время на инициализацию
    
    // 6. Storage
    DBG_PRINTLN("[SETUP] Storage...");
    storageStart();
    delay(50);
    
    // 7. Simulator или RealEngine
#if REAL_ENGINE_ENABLED
    DBG_PRINTLN("[SETUP] Real Engine...");
    realEngineStart();
#else
    DBG_PRINTLN("[SETUP] Simulator...");
    simulatorStart();
#endif
    delay(50);
    
    // 8. Calculator
    DBG_PRINTLN("[SETUP] Calculator...");
    calculatorStart();
    delay(50);
    
    // 9. Protocol
    DBG_PRINTLN("[SETUP] Protocol...");
    protocolStart();
    delay(50);
    
    // 10. K-Line
    DBG_PRINTLN("[SETUP] K-Line...");
    klineStart();
    delay(50);
    
    // 11. Climate
    DBG_PRINTLN("[SETUP] Climate...");
    climateStart();
    delay(50);
    
    // 12. OLED (опционально)
#if OLED_ENABLED
    DBG_PRINTLN("[SETUP] OLED...");
    oledStart();
    delay(50);
#endif
    
    DBG_NEWLINE();
    DBG_PRINTLN("=== Setup Complete ===");
    DBG_PRINTF("Free heap: %u bytes", ESP.getFreeHeap());
    DBG_NEWLINE();
}

// =============================================================================
// loop() — мониторинг задач
// =============================================================================

// Cooldown для рестартов — предотвращает "смертельную спираль"
static unsigned long lastRestartKline = 0;
static unsigned long lastRestartClimate = 0;
static unsigned long lastRestartStorage = 0;
#if OLED_ENABLED
static unsigned long lastRestartDisplay = 0;
#endif
#define RESTART_COOLDOWN_MS 5000  // 5 сек между рестартами

void loop() {

    // ПЕРВЫМ ДЕЛОМ — если OTA активен, ничего не делаем!
    if (otaIsInProgress()) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
        return;
    }

    static unsigned long lastCheck = 0;
    unsigned long now = millis();
    
    // Проверяем состояние задач каждые 500 мс
    if (now - lastCheck >= 500) {
        lastCheck = now;
        
        // НЕ перезапускать задачи, если OTA активен!
        if (otaIsInProgress()) {
            return;
        }
        
        // --- КРИТИЧНЫЕ МОДУЛИ ---
        
        // Engine (Simulator или Real)
#if REAL_ENGINE_ENABLED
        if (!realEngineIsRunning()) {
            DBG_PRINTLN("[LOOP] CRITICAL: RealEngine crashed! Restarting...");
            restartRealEngine();
        }
#else
        if (!simulatorIsRunning()) {
            DBG_PRINTLN("[LOOP] CRITICAL: Simulator crashed! Restarting...");
            restartSimulator();
        }
#endif
        
        // Storage
        if (!storageIsRunning() && (now - lastRestartStorage >= RESTART_COOLDOWN_MS)) {
            lastRestartStorage = now;
            DBG_PRINTLN("[LOOP] Storage crashed! Restarting...");
            restartStorage();
        }
        
        // Calculator
        if (!calculatorIsRunning()) {
            DBG_PRINTLN("[LOOP] Calculator crashed! Restarting...");
            restartCalculator();
        }
        
        // Protocol
        if (!protocolIsRunning()) {
            DBG_PRINTLN("[LOOP] Protocol crashed! Restarting...");
            restartProtocol();
        }
        
        // --- СЕРВИСНЫЕ МОДУЛИ (с cooldown) ---
        
        // K-Line
        if (!klineIsRunning() && (now - lastRestartKline >= RESTART_COOLDOWN_MS)) {
            lastRestartKline = now;
            DBG_PRINTLN("[LOOP] K-Line crashed! Restarting...");
            restartKline();
        }
        
        // Climate
        if (!climateIsRunning() && (now - lastRestartClimate >= RESTART_COOLDOWN_MS)) {
            lastRestartClimate = now;
            DBG_PRINTLN("[LOOP] Climate crashed! Restarting...");
            restartClimate();
        }
        
        // OLED
#if OLED_ENABLED
        if (!oledIsRunning() && (now - lastRestartDisplay >= RESTART_COOLDOWN_MS)) {
            lastRestartDisplay = now;
            DBG_PRINTLN("[LOOP] OLED crashed! Restarting...");
            restartDisplay();
        }
#endif
    }
    
    vTaskDelay(100 / portTICK_PERIOD_MS);
}