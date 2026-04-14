// -----------------------------------------------------------------------------
// main.cpp
// Точка входа и Loop-диспетчер процессов.
//
// Порядок инициализации:
//   1. DataRouter → 2. Storage → 3. Simulator → 4. Calculator
//   5. Protocol → 6. BT Transport → 7. K-Line → 8. Climate → 9. OLED
//
// Loop: heartbeat-мониторинг, перезапуск при падении.
//
// ВЕРСИЯ: 6.1.0 — K-Line и Climate подключены
// -----------------------------------------------------------------------------

#include <Arduino.h>
#include "data_router.h"
#include "topics.h"
#include "app_config.h"
#include "bt_transport.h"

// Внешние декларации
extern void simulatorStart();     extern void simulatorStop();     extern bool simulatorIsRunning();
extern void storageStart();       extern void storageStop();       extern bool storageIsRunning();
extern void calculatorStart();    extern void calculatorStop();    extern bool calculatorIsRunning();
extern void protocolStart();      extern void protocolStop();      extern bool protocolIsRunning();
extern void klineStart();         extern void klineStop();         extern bool klineIsRunning();
extern void climateStart();       extern void climateStop();       extern bool climateIsRunning();
extern void realEngineStart();    extern void realEngineStop();    extern bool realEngineIsRunning();
#if OLED_ENABLED
extern void oledStart();          extern void oledStop();          extern bool oledIsRunning();
#endif

// OTA Task — ленивый запуск при ota_update (не в setup())
extern bool otaIsInProgress();

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
// setup
// =============================================================================

void setup() {
    Serial.begin(115200);
    delay(2000);

    Serial.println();
    Serial.println("=== Car BKV2 Starting ===");
    Serial.printf("Firmware: v%s\n", FW_VERSION_STR);
    Serial.printf("Note: %s\n", FW_VERSION_NOTE);
    Serial.println("Architecture: DataRouter (typed topics, module-owned queues)\n");

    // 1. DataRouter
    Serial.println("[SETUP] 1/9: DataRouter...");
    DataRouter::getInstance().begin();
    delay(50);

    // 2. BT Transport — ЗАПУСКАЕМ РАНЬШЕ ВСЕХ ЗАДАЧ (до фрагментации кучи)
    Serial.println("[SETUP] 2/9: BT Transport...");
    btTransportStart("Car Simulator");
    delay(100);

    // 3. Storage (публикует данные — кэш для остальных)
    Serial.println("[SETUP] 3/9: Storage...");
    storageStart();
    delay(100);

    // 3. Engine (Simulator или Real)
#if REAL_ENGINE_ENABLED
    Serial.println("[SETUP] 4/9: Real Engine...");
    realEngineStart();
#else
    Serial.println("[SETUP] 4/9: Simulator...");
    simulatorStart();
#endif
    delay(100);

    // 4. Calculator
    Serial.println("[SETUP] 5/9: Calculator...");
    calculatorStart();
    delay(100);

    // 5. Protocol
    Serial.println("[SETUP] 6/9: Protocol...");
    protocolStart();
    delay(100);

    // 6. K-Line
    Serial.println("[SETUP] 7/9: K-Line...");
    klineStart();
    delay(100);

    // 7. Climate
    Serial.println("[SETUP] 8/9: Climate...");
    climateStart();
    delay(100);

    // 8. OLED
#if OLED_ENABLED
    Serial.println("[SETUP] 9/9: OLED...");
    oledStart();
#else
    Serial.println("[SETUP] 7/7: Complete (no OLED)");
#endif

#if OLED_ENABLED
    Serial.println("\n=== Setup Complete (Sim/Storage/Calc/Proto/BT/KLine/Climate/OLED) ===\n");
#else
    Serial.println("\n=== Setup Complete (Sim/Storage/Calc/Proto/BT/KLine/Climate) ===\n");
#endif

    // Даём BT стеку стабилизироваться перед рестартами задач
    delay(1000);
    Serial.printf("[SETUP] Free heap: %u bytes\n", (unsigned)ESP.getFreeHeap());
}

// =============================================================================
// loop — Диспетчер процессов (упрощённый)
// =============================================================================

// Cooldown для рестартов — предотвращает "смертельную спираль"
static unsigned long lastRestartKline = 0, lastRestartClimate = 0, lastRestartStorage = 0;
#if OLED_ENABLED
static unsigned long lastRestartDisplay = 0;
#endif
#define RESTART_COOLDOWN_MS 2000  // Мин. 2 сек между рестартами

// ⚠️ ВРЕМЕННО: отключён restart задач для отладки crash при подключении BT
#define DISABLE_LOOP_RESTARTS  1

void loop() {
    static unsigned long lastCheck = 0;
    unsigned long now = millis();

    // Heartbeat-мониторинг всех модулей
    if (now - lastCheck >= 100) {
        lastCheck = now;

        // НЕ перезапускать задачи если OTA активен!
        if (otaIsInProgress()) {
            vTaskDelay(50 / portTICK_PERIOD_MS);
            return;
        }

#if DISABLE_LOOP_RESTARTS
        // --- Все рестарты отключены для теста ---
        (void)lastRestartKline; (void)lastRestartClimate; (void)lastRestartStorage;
        (void)RESTART_COOLDOWN_MS;
#else
        // Критичные модули — Engine (Sim или Real)
#if REAL_ENGINE_ENABLED
        if (!realEngineIsRunning()) {
            Serial.println("[LOOP] CRITICAL: RealEngine crashed! Restarting...");
            restartRealEngine();
        }
#else
        if (!simulatorIsRunning()) {
            Serial.println("[LOOP] CRITICAL: Simulator crashed! Restarting...");
            restartSimulator();
        }
#endif
        if (!storageIsRunning() && (now - lastRestartStorage >= RESTART_COOLDOWN_MS)) {
            lastRestartStorage = now;
            Serial.println("[LOOP] HIGH: Storage crashed! Restarting...");
            restartStorage();
        }
        if (!calculatorIsRunning()) {
            Serial.println("[LOOP] CRITICAL: Calculator crashed! Restarting...");
            restartCalculator();
        }
        if (!protocolIsRunning()) {
            Serial.println("[LOOP] CRITICAL: Protocol crashed! Restarting...");
            restartProtocol();
        }

        // Сервисные модули (с cooldown)
        if (!klineIsRunning() && (now - lastRestartKline >= RESTART_COOLDOWN_MS)) {
            lastRestartKline = now;
            Serial.println("[LOOP] LOW: K-Line crashed! Restarting...");
            restartKline();
        }
        if (!climateIsRunning() && (now - lastRestartClimate >= RESTART_COOLDOWN_MS)) {
            lastRestartClimate = now;
            Serial.println("[LOOP] LOW: Climate crashed! Restarting...");
            restartClimate();
        }
#if OLED_ENABLED
        if (!oledIsRunning() && (now - lastRestartDisplay >= RESTART_COOLDOWN_MS)) {
            lastRestartDisplay = now;
            Serial.println("[LOOP] LOW: OLED crashed! Restarting...");
            restartDisplay();
        }
#endif
#endif // DISABLE_LOOP_RESTARTS
    }

    vTaskDelay(50 / portTICK_PERIOD_MS);
}
