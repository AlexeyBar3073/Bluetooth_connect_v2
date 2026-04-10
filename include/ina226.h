// -----------------------------------------------------------------------------
// ina226.h
// Драйвер INA226 (I2C): напряжение бортсети + уровень топлива (поплавок-реостат).
//
// Подключение:
//   SDA/SCL — общая I2C шина (GPIO 21/22)
//   INA226 #1 (addr 0x40): bus voltage → бортсеть, shunt → поплавок топлива
//
// Регистры INA226:
//   0x00 — Configuration
//   0x01 — Shunt Voltage (±81.92 мВ, LSB = 2.5 мкВ)
//   0x02 — Bus Voltage    (0–36 В, LSB = 1.25 мВ)
//   0x03 — Power
//   0x04 — Current
//   0x05 — Calibration
//
// ВЕРСИЯ: 6.2.0
// -----------------------------------------------------------------------------

#ifndef INA226_H
#define INA226_H

#include <Arduino.h>
#include <Wire.h>

class INA226 {
public:
    // addr: 0x40 (A0=GND, A1=GND), 0x41 (A0=V+, A1=GND) и т.д.
    explicit INA226(uint8_t addr = 0x40);

    // Инициализация I2C
    bool begin(TwoWire& wire = Wire, float shunt_ohm = 0.1f);

    // Напряжение бортсети (мВ, до 36 В без делителя)
    float readBusVoltage();

    // Напряжение на шунте (мВ, ±81.92 мВ)
    float readShuntVoltage();

    // Ток через шунт (мА) — shunt_ohm задаётся в configure()
    float readCurrent();

    // Калибровка: shunt_ohm — сопротивление шунта (Ом)
    void configure(float shunt_ohm = 0.1f);

    // Чтение сырого регистра
    uint16_t readRegister(uint8_t reg);

    // Запись регистра
    void writeRegister(uint8_t reg, uint16_t value);

private:
    uint8_t _addr;
    TwoWire* _wire;
    float _shuntOhm;
    float _currentLSB_mA;

    static constexpr float SHUNT_LSB_uV  = 2.5f;    // 2.5 мкВ на единицу
    static constexpr float BUS_LSB_mV    = 1.25f;   // 1.25 мВ на единицу
};

#endif // INA226_H
