// -----------------------------------------------------------------------------
// protocol_task.cpp
// Протокол связи с Android по Bluetooth (JSON-команды, фракционная телеметрия).
//
// Архитектура (Queue-based):
// - Подписка на EnginePack, TripPack, KlinePack, ClimatePack, SettingsPack → очереди
// - Чтение из очередей → обновление локального кэша телеметрии
// - Подписка на TOPIC_MSG_INCOMING → парсинг JSON-команд, извлечение msg_id
// - Фракционная отправка JSON: FAST 100мс, TRIP 500мс, SERVICE 1000мс
// - Квитанции (msg_id → ack_id): только Protocol знает про них
//
// -----------------------------------------------------------------------------
// ПРАВИЛА ФАЙЛА:
// -----------------------------------------------------------------------------
// ✅ можно:
//   - Добавлять новые команды
//   - Менять поля JSON
//   - Менять частоту фракционной отправки
//
// ❌ нельзя:
//   - Блокировать >10 мс
//   - Использовать String для outgoing JSON
//   - Передавать msg_id/ack_id другим модулям (это территория Protocol)
//
// ВЕРСИЯ: 5.1.0 — MAJOR: KlinePack/ClimatePack, квитанции только в Protocol
// -----------------------------------------------------------------------------

#include "protocol_task.h"
#include "data_router.h"
#include "topics.h"
#include "packets.h"
#include "commands.h"
#include "bt_transport.h"
#include "app_config.h"
#include <ArduinoJson.h>

// =============================================================================
// Конфигурация
// =============================================================================
#define FAST_MS    100

// =============================================================================
// Глобальные переменные
// =============================================================================

static TaskHandle_t  taskHandle       = NULL;
static bool          isRunningFlag    = false;
static unsigned long lastHeartbeat    = 0;
static bool          isStreamingActive = false;

// --- Квитанция доставки (msg_id → ack_id) ---
// Только Protocol знает про эти поля.
// При получении команды: сохраняем msg_id
// При отправке любого исходящего: если lastMsgId != 0 → добавляем ack_id → обнуляем
static int lastMsgId = 0;

// --- Кэш телеметрии (обновляется из очередей пакетов) ---
static struct {
    // EnginePack
    float   speed = 0, rpm = 0, voltage = 0, inst_fuel = 0;
    float   fuel_level = 0;
    bool    eng = false, hl = false;

    // TripPack
    double  odo = 0;
    float   trip_a = 0, fuel_a = 0, trip_b = 0, fuel_b = 0;
    float   trip_cur = 0, fuel_cur = 0, fuel = 0, avg = 0, avg_total = 0;

    // KlinePack (AKPP + диагностика)
    float   t_cool = 0, t_atf = 0;
    float   voltage_kl = 0, fuel_pct = 0, out_shaft = 0;
    bool    tcc = false;
    uint8_t selector = 3;    // D
    uint8_t gear = 0;
    char    dtc[64] = "";

    // ClimatePack
    float   t_int = 0, t_ext = 0;
    bool    tire = false, wash = false;
} tel;

// --- Кэш настроек ---
static struct {
    float   tank = 60.0f, inj_perf = 250.0f, spd_sig = 3.0f;
    uint8_t inj_cnt = 4, kl_proto = 0;
} cfg;

static char outBuffer[512];

// =============================================================================
// processEnginePack
// =============================================================================
static void processEnginePack(QueueHandle_t q) {
    EnginePack p;
    if (xQueueReceive(q, &p, 0) == pdTRUE) {
        tel.speed = p.speed;
        tel.rpm = p.rpm;
        tel.voltage = p.voltage;
        tel.eng = p.engine_running;
        tel.hl = p.parking_lights;
        tel.inst_fuel = p.instant_fuel;
        tel.fuel_level = p.fuel_level_sensor;
    }
}

// =============================================================================
// processTripPack
// =============================================================================
static void processTripPack(QueueHandle_t q) {
    TripPack p;
    if (xQueueReceive(q, &p, 0) == pdTRUE) {
        tel.odo = p.odo;
        tel.trip_a = p.trip_a;
        tel.fuel_a = p.fuel_trip_a;
        tel.trip_b = p.trip_b;
        tel.fuel_b = p.fuel_trip_b;
        tel.trip_cur = p.trip_cur;
        tel.fuel_cur = p.fuel_cur;
        tel.fuel = p.fuel_level;
        tel.avg = p.avg_consumption;
        tel.avg_total = p.avg_total;
    }
}

// =============================================================================
// processKlinePack
// =============================================================================
static void processKlinePack(QueueHandle_t q) {
    KlinePack p;
    if (xQueueReceive(q, &p, 0) == pdTRUE) {
        tel.t_cool = p.coolant_temp;
        tel.t_atf = p.atf_temp;
        tel.voltage_kl = p.voltage;
        tel.fuel_pct = p.fuel_percent;
        tel.out_shaft = p.output_shaft_rpm;
        tel.tcc = p.tcc_lockup;
        tel.selector = p.selector_position;
        tel.gear = p.current_gear;
        strlcpy(tel.dtc, p.dtc_codes, sizeof(tel.dtc));
    }
}

// =============================================================================
// processClimatePack
// =============================================================================
static void processClimatePack(QueueHandle_t q) {
    ClimatePack p;
    if (xQueueReceive(q, &p, 0) == pdTRUE) {
        tel.t_int = p.interior_temp;
        tel.t_ext = p.exterior_temp;
        tel.tire = p.tire_pressure;
        tel.wash = p.washer_level;
    }
}

// =============================================================================
// processSettingsPack
// =============================================================================
static void processSettingsPack(QueueHandle_t q) {
    SettingsPack p;
    if (xQueueReceive(q, &p, 0) == pdTRUE) {
        cfg.tank = p.tank_capacity;
        cfg.inj_perf = p.injector_flow;
        cfg.inj_cnt = p.injector_count;
        cfg.spd_sig = p.pulses_per_meter;
        cfg.kl_proto = p.kline_protocol;
    }
}

// =============================================================================
// Forward declarations
// =============================================================================
static void buildFastJson(JsonDocument& doc);
static void addTripFields(JsonDocument& doc);
static void addServiceFields(JsonDocument& doc);
static void injectAckId(JsonDocument& doc);
static void publishOutgoing(JsonDocument& doc);

// =============================================================================
// injectAckId: Подмешивает ack_id если есть квитанция, обнуляет lastMsgId
// =============================================================================
static void injectAckId(JsonDocument& doc) {
    if (lastMsgId != 0) {
        doc["ack_id"] = lastMsgId;
        lastMsgId = 0;
    }
}

// =============================================================================
// publishOutgoing: Сериализует JSON и отправляет через DataRouter
// =============================================================================

static void publishOutgoing(JsonDocument& doc) {
    serializeJson(doc, outBuffer, sizeof(outBuffer));
    strlcat(outBuffer, "\n", sizeof(outBuffer));
    outBuffer[sizeof(outBuffer) - 1] = '\0';

    // Отправляем напрямую через BT
    btSend(outBuffer);

    // Все JSON дублируем в шину (для отладки)
    DataRouter::getInstance().publishString(TOPIC_MSG_OUTGOING, outBuffer);
}

// =============================================================================
// processIncoming: Обработка входящих JSON-команд
// =============================================================================
//
// Только Protocol знает про msg_id/ack_id.
// Извлекает msg_id, сохраняет локально, обрабатывает команду,
// отправляет ACK (квитанция подмешается к ближайшему исходящему).
//
static void processIncoming(QueueHandle_t q) {
    char rxBuffer[256];
    while (xQueueReceive(q, rxBuffer, 0) == pdTRUE) {
        Serial.printf("[Protocol] RX JSON: %s\n", rxBuffer);

        JsonDocument doc;
        if (deserializeJson(doc, rxBuffer)) {
            // Невалидный JSON
            JsonDocument err;
            err["error"] = "Invalid JSON";
            publishOutgoing(err);
            continue;
        }

        const char* cmd = doc["command"];
        if (!cmd) cmd = doc["cmd"];
        if (!cmd) {
            continue;
        }

        // Извлекаем msg_id — квитанция доставки
        JsonVariant msgIdVar = doc["msg_id"];
        if (!msgIdVar.isNull() && msgIdVar.is<int>()) {
            lastMsgId = msgIdVar.as<int>();
        }

        Serial.printf("[Protocol] Command='%s', msg_id=%d\n", cmd, lastMsgId);

        // --- get_cfg: ответ из локального кэша настроек ---
        if (strcmp(cmd, "get_cfg") == 0) {
            JsonDocument resp;
            injectAckId(resp);
            JsonObject c = resp["cfg"].to<JsonObject>();
            c["tV"] = cfg.tank; c["iPerf"] = cfg.inj_perf;
            c["iCnt"] = cfg.inj_cnt; c["sSig"] = cfg.spd_sig;
            c["kPrt"] = cfg.kl_proto; c["fw"] = FW_VERSION_STR;
            publishOutgoing(resp);
            continue;
        }

        // --- set_cfg: публикуем SettingsPack в шину ---
        if (strcmp(cmd, "set_cfg") == 0) {
            JsonObject data = doc["data"];
            if (!data) data = doc.as<JsonObject>();

            SettingsPack pack;
            memset(&pack, 0, sizeof(pack));
            pack.version = 1;
            pack.tank_capacity = cfg.tank;
            pack.injector_count = cfg.inj_cnt;
            pack.injector_flow = cfg.inj_perf;
            pack.pulses_per_meter = cfg.spd_sig;
            pack.kline_protocol = cfg.kl_proto;

            if (data["tV"]) pack.tank_capacity = data["tV"].as<float>();
            if (data["iPerf"]) pack.injector_flow = data["iPerf"].as<float>();
            if (data["iCnt"]) pack.injector_count = data["iCnt"].as<int>();
            if (data["sSig"]) pack.pulses_per_meter = data["sSig"].as<float>();
            if (data["kPrt"]) pack.kline_protocol = data["kPrt"].as<int>();

            DataRouter::getInstance().publishPacket(TOPIC_SETTINGS_PACK, &pack, sizeof(pack));

            // ACK (квитанция подмешается)
            JsonDocument resp;
            injectAckId(resp);
            publishOutgoing(resp);
            continue;
        }

        // --- start/stop_telemetry ---
        if (strcmp(cmd, "start_telemetry") == 0) {
            // ACK сразу (квитанция подмешается)
            JsonDocument resp;
            injectAckId(resp);
            publishOutgoing(resp);

            isStreamingActive = true;
            Serial.println("[Protocol] Streaming STARTED");
            continue;
        }
        else if (strcmp(cmd, "stop_telemetry") == 0) {
            JsonDocument resp;
            injectAckId(resp);
            publishOutgoing(resp);

            isStreamingActive = false;
            Serial.println("[Protocol] Streaming STOPPED");
            continue;
        }

        // --- Команды в шину (enum Command) ---
        Command enumCmd = CMD_NONE;

        if (strcmp(cmd, "reset_trip_a") == 0)       enumCmd = CMD_RESET_TRIP_A;
        else if (strcmp(cmd, "reset_trip_b") == 0)  enumCmd = CMD_RESET_TRIP_B;
        else if (strcmp(cmd, "reset_avg") == 0)     enumCmd = CMD_RESET_AVG;
        else if (strcmp(cmd, "full_tank") == 0)     enumCmd = CMD_FULL_TANK;
        else if (strcmp(cmd, "kl_get_dtc") == 0)    enumCmd = CMD_KL_GET_DTC;
        else if (strcmp(cmd, "kl_clear_dtc") == 0)  enumCmd = CMD_KL_CLEAR_DTC;
        else if (strcmp(cmd, "kl_reset_adapt") == 0) enumCmd = CMD_KL_RESET_ADAPT;
        else if (strcmp(cmd, "kl_pump_atf") == 0)   enumCmd = CMD_KL_PUMP_ATF;
        else if (strcmp(cmd, "kl_detect_protocol") == 0) enumCmd = CMD_KL_DETECT_PROTO;
        else if (strcmp(cmd, "correct_odo") == 0) {
            // Параметрическая команда — публикуем в отдельный топик
            // Android шлёт: {"data": 123456} или {"data":{"value":123456}}
            int odo_value = 0;
            if (doc["data"].is<int>()) {
                odo_value = doc["data"].as<int>();
            } else if (doc["data"]["value"].is<int>()) {
                odo_value = doc["data"]["value"].as<int>();
            }
            DataRouter::getInstance().publish(TOPIC_CORRECT_ODO, odo_value);
            Serial.printf("[Protocol] correct_odo: %d km\n", odo_value);
            // ACK подмешается в ближайшее исходящее
            continue;
        }
        else if (strcmp(cmd, "set_cfg") == 0) {
            // Обработано выше — не должно попасть сюда
            continue;
        }
        else {
            // Неизвестная команда
            JsonDocument resp;
            injectAckId(resp);
            resp["error"] = "Unknown command";
            publishOutgoing(resp);
            continue;
        }

        // Публикуем enum Command в шину (1 байт — без msg_id, msg_id знает только Protocol)
        DataRouter::getInstance().publish(TOPIC_CMD, enumCmd);

        // ACK (квитанция подмешается)
        JsonDocument resp;
        injectAckId(resp);
        publishOutgoing(resp);
    }
}

// =============================================================================
// buildFastJson: Заполняет документ только FAST-полями
// =============================================================================

static void buildFastJson(JsonDocument& doc) {
    JsonObject t = doc["tel"].to<JsonObject>();
    t["spd"] = (int)tel.speed;
    t["rpm"] = (int)tel.rpm;
    t["vlt"] = roundf(tel.voltage * 10) / 10;
    t["eng"] = (int)tel.eng;
    t["hl"] = (int)tel.hl;
    const char* selStr[] = {"P", "R", "N", "D", "3", "2", "L"};
    const char* base = (tel.selector <= 6) ? selStr[tel.selector] : "D";
    char selBuf[8];
    if (tel.gear > 0 && tel.selector == 3) {
        snprintf(selBuf, sizeof(selBuf), "%s%d", base, tel.gear);
    } else {
        snprintf(selBuf, sizeof(selBuf), "%s", base);
    }
    t["sel"] = selBuf;
    t["tcc"] = (int)tel.tcc;
    t["fuel"] = roundf(tel.fuel_level * 10) / 10;
}

// =============================================================================
// addTripFields: Добавляет TRIP-поля к существующему документу
// =============================================================================

static void addTripFields(JsonDocument& doc) {
    JsonObject t = doc["tel"];
    t["odo"] = (int)tel.odo;
    t["trip_a"] = roundf(tel.trip_a * 10) / 10;
    t["fuel_a"] = roundf(tel.fuel_a * 10) / 10;
    t["trip_b"] = roundf(tel.trip_b * 10) / 10;
    t["fuel_b"] = roundf(tel.fuel_b * 10) / 10;
    t["trip_cur"] = roundf(tel.trip_cur * 10) / 10;
    t["fuel_cur"] = roundf(tel.fuel_cur * 10) / 10;
    t["inst"] = roundf(tel.inst_fuel * 10) / 10;
    t["avg_cur"] = roundf(tel.avg * 10) / 10;
    float avgToDisplay = (tel.avg_total > 0.0f) ? tel.avg_total : tel.avg;
    t["avg"] = roundf(avgToDisplay * 10) / 10;
}

// =============================================================================
// addServiceFields: Добавляет SERVICE-поля (KlinePack + ClimatePack)
// =============================================================================

static void addServiceFields(JsonDocument& doc) {
    JsonObject t = doc["tel"];
    t["t_cool"] = roundf(tel.t_cool * 10) / 10;
    t["t_atf"] = roundf(tel.t_atf * 10) / 10;
    t["dtc"] = tel.dtc;
    t["t_int"] = roundf(tel.t_int * 10) / 10;
    t["t_ext"] = roundf(tel.t_ext * 10) / 10;
    t["tire"] = (int)tel.tire;
    t["wash"] = (int)tel.wash;
}

// =============================================================================
// protocolTask — Главная задача
// =============================================================================

void protocolTask(void* parameter) {
    (void)parameter;
    isRunningFlag = true;
    DataRouter& dr = DataRouter::getInstance();

    // Создаём очереди для пакетов (модуль создаёт, DataRouter регистрирует)
    QueueHandle_t engineQ   = xQueueCreate(1, sizeof(EnginePack));
    QueueHandle_t tripQ     = xQueueCreate(1, sizeof(TripPack));
    QueueHandle_t klineQ    = xQueueCreate(1, sizeof(KlinePack));
    QueueHandle_t climateQ  = xQueueCreate(1, sizeof(ClimatePack));
    QueueHandle_t settingsQ = xQueueCreate(1, sizeof(SettingsPack));

    dr.subscribe(TOPIC_ENGINE_PACK,   engineQ,   QueuePolicy::OVERWRITE);
    dr.subscribe(TOPIC_TRIP_PACK,     tripQ,     QueuePolicy::OVERWRITE);
    dr.subscribe(TOPIC_KLINE_PACK,    klineQ,    QueuePolicy::OVERWRITE);
    dr.subscribe(TOPIC_CLIMATE_PACK,  climateQ,  QueuePolicy::OVERWRITE);
    dr.subscribe(TOPIC_SETTINGS_PACK, settingsQ, QueuePolicy::OVERWRITE);

    // Подписка на входящие (OVERWRITE, depth=1 — всегда актуальное, повторная отправка при необходимости)
    QueueHandle_t incomingQ = xQueueCreate(1, 128);
    dr.subscribe(TOPIC_MSG_INCOMING, incomingQ, QueuePolicy::OVERWRITE);

    Serial.println("[Protocol] Task started (DataRouter, Fractional: FAST 100ms, TRIP 500ms, SERVICE 1000ms)");

    unsigned long lastSend = 0;
    int counter = 0;

    while (1) {
        lastHeartbeat = millis();

        // Чтение очередей пакетов
        if (engineQ)   processEnginePack(engineQ);
        if (tripQ)     processTripPack(tripQ);
        if (klineQ)    processKlinePack(klineQ);
        if (climateQ)  processClimatePack(climateQ);
        if (settingsQ) processSettingsPack(settingsQ);

        // Обработка входящих команд
        if (incomingQ) processIncoming(incomingQ);

        // Фракционная отправка телеметрии (ТЗ п.5.1–5.6)
        if (!isStreamingActive) { vTaskDelay(10 / portTICK_PERIOD_MS); continue; }

        unsigned long now = millis();
        if (now - lastSend >= 100) {
            lastSend = now;
            counter++;

            if (!btIsConnected()) continue;

            JsonDocument doc;
            buildFastJson(doc);  // FAST: spd, rpm, vlt, eng, hl, gear, sel, tcc, fuel

            if (counter % 5 == 0) addTripFields(doc);    // TRIP: каждые 500мс
            if (counter % 10 == 0) addServiceFields(doc); // SERVICE: каждые 1000мс

            // Подмешиваем ack_id если есть квитанция
            injectAckId(doc);

            publishOutgoing(doc);

            // Отладка: первые 3 сообщения и каждое 100-е — выводим полный JSON
            static int logCount = 0;
            if (++logCount <= 3 || logCount % 100 == 0) {
                Serial.printf("[Protocol] >>> TX #%d counter=%d, len=%zu, json=%s", logCount, counter, strlen(outBuffer), outBuffer);
            }
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

// =============================================================================
// Управление
// =============================================================================

void protocolStart() {
    if (!taskHandle) {
        xTaskCreatePinnedToCore(protocolTask, "Protocol", TASK_STACK_PROTOCOL, NULL, 3, &taskHandle, 1);
        Serial.println("[Protocol] Started");
    }
}

void protocolStop() {
    if (taskHandle) {
        vTaskDelete(taskHandle);
        taskHandle = NULL;
        isRunningFlag = false;
        Serial.println("[Protocol] Stopped");
    }
}

bool protocolIsRunning() {
    return isRunningFlag && (millis() - lastHeartbeat) < 3000;
}


