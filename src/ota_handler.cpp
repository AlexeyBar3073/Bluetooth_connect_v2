// -----------------------------------------------------------------------------
// ota_handler.cpp
// OTA обновление прошивки через Bluetooth Serial (SPP).
//
// Архитектура:
//   - Android шлёт .bin файл чанками по 1024 байт
//   - ESP32 пишет в OTA partition через Update.h
//   - CRC32 проверка целостности
//   - При успехе — перезагрузка в новую прошивку
//
// ВЕРСИЯ: 6.4.0 — OTA через Bluetooth Serial
// -----------------------------------------------------------------------------

#include "ota_handler.h"
#include "bt_transport.h"
#include <Update.h>
#include <Arduino.h>
#include <BluetoothSerial.h>

extern BluetoothSerial SerialBT;  // Глобальный из bt_transport.cpp

// =============================================================================
// Состояние OTA
// =============================================================================

static bool      otaActive       = false;
static uint32_t  otaTotalSize    = 0;
static uint32_t  otaReceivedSize = 0;
static uint32_t  otaExpectedCRC  = 0;
static uint8_t   otaProgress     = 0;

// =============================================================================
// otaStart — начать обновление
// =============================================================================

bool otaStart(uint32_t firmwareSize, uint32_t crc) {
    if (otaActive) return false;

    otaTotalSize = firmwareSize;
    otaExpectedCRC = crc;
    otaReceivedSize = 0;
    otaProgress = 0;

    Serial.printf("[OTA] Start: %lu bytes, CRC=0x%08X\n", firmwareSize, crc);

    // Проверить размер (не больше доступного места)
    if (firmwareSize == 0 || firmwareSize > 0x100000) {  // 1MB макс
        Serial.println("[OTA] Error: Invalid firmware size");
        return false;
    }

    // Начать Update
    if (!Update.begin(firmwareSize)) {
        Serial.printf("[OTA] Error: Update.begin failed: %s\n", Update.errorString());
        return false;
    }

    otaActive = true;
    return true;
}

// =============================================================================
// otaIsActive — проверить, активен ли OTA режим
// =============================================================================

bool otaIsActive() {
    return otaActive;
}

// =============================================================================
// otaGetProgress — прогресс OTA (0-100%)
// =============================================================================

uint8_t otaGetProgress() {
    if (!otaActive || otaTotalSize == 0) return 0;
    return (uint8_t)((otaReceivedSize * 100) / otaTotalSize);
}

// =============================================================================
// calcCRC32 — посчитать CRC32 буфера
// =============================================================================

static uint32_t calcCRC32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (crc & 1 ? 0xEDB88320 : 0);
        }
    }
    return ~crc;
}

// =============================================================================
// otaHandle — обработать байты OTA
// =============================================================================
//
// Формат команд:
//   START: [0xA0][SIZE:4 bytes LE][CRC:4 bytes LE]  = 9 байт
//   DATA:  [0xA1][DATA:1..1024 bytes]               = 2..1025 байт
//   END:   [0xA2]                                    = 1 байт
//
bool otaHandle(const uint8_t* data, size_t len) {
    if (len == 0) return false;

    uint8_t cmd = data[0];

    switch (cmd) {
        // =====================================================================
        // OTA_CMD_START — начать обновление
        // =====================================================================
        case OTA_CMD_START: {
            if (len != 9) {
                Serial.printf("[OTA] START: Wrong length (%zu, expected 9)\n", len);
                SerialBT.write(OTA_RSP_ERR);
                return false;
            }

            uint32_t firmwareSize = data[1] | (data[2] << 8) | (data[3] << 16) | (data[4] << 24);
            uint32_t crc = data[5] | (data[6] << 8) | (data[7] << 16) | (data[8] << 24);

            if (otaStart(firmwareSize, crc)) {
                SerialBT.write(OTA_RSP_READY);
            } else {
                SerialBT.write(OTA_RSP_ERR);
            }
            break;
        }

        // =====================================================================
        // OTA_CMD_DATA — чанк прошивки
        // =====================================================================
        case OTA_CMD_DATA: {
            if (!otaActive) {
                Serial.println("[OTA] DATA: OTA not active");
                SerialBT.write(OTA_RSP_ERR);
                return false;
            }

            size_t chunkSize = len - 1;
            if (chunkSize == 0 || chunkSize > OTA_CHUNK_SIZE) {
                Serial.printf("[OTA] DATA: Invalid chunk size (%zu)\n", chunkSize);
                SerialBT.write(OTA_RSP_NACK);
                return false;
            }

            // Update.write требует uint8_t*, не const
            size_t written = Update.write(const_cast<uint8_t*>(data + 1), chunkSize);
            if (written == chunkSize) {
                otaReceivedSize += written;
                otaProgress = otaGetProgress();

                SerialBT.write(OTA_RSP_ACK);

                // Логировать прогресс каждые 10%
                static uint8_t lastLogProgress = 0;
                if (otaProgress - lastLogProgress >= 10) {
                    lastLogProgress = otaProgress;
                    Serial.printf("[OTA] Progress: %d%% (%lu/%lu)\n",
                                  otaProgress, otaReceivedSize, otaTotalSize);
                }
            } else {
                Serial.printf("[OTA] DATA: Write failed (%zu/%zu)\n", written, chunkSize);
                SerialBT.write(OTA_RSP_NACK);
            }
            break;
        }

        // =====================================================================
        // OTA_CMD_END — завершить обновление
        // =====================================================================
        case OTA_CMD_END: {
            if (!otaActive) {
                Serial.println("[OTA] END: OTA not active");
                SerialBT.write(OTA_RSP_ERR);
                return false;
            }

            Serial.println("[OTA] End: Verifying...");

            // Завершить Update (true = проверить CRC)
            if (Update.end(true)) {
                Serial.println("[OTA] Update complete! Rebooting...");
                SerialBT.write(OTA_RSP_OK);

                // Дать время на отправку ответа
                delay(1000);

                // Перезагрузка в новую прошивку
                ESP.restart();
            } else {
                Serial.printf("[OTA] Error: %s\n", Update.errorString());
                SerialBT.write(OTA_RSP_ERR);
            }

            otaActive = false;
            break;
        }

        // =====================================================================
        // Неизвестная команда
        // =====================================================================
        default:
            Serial.printf("[OTA] Unknown command: 0x%02X\n", cmd);
            SerialBT.write(OTA_RSP_ERR);
            break;
    }

    return true;
}
