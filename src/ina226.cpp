// -----------------------------------------------------------------------------
// ina226.cpp
// Минимальный драйвер INA226 без внешних зависимостей.
// -----------------------------------------------------------------------------

#include "ina226.h"

// Регистры INA226
#define REG_CONFIG      0x00
#define REG_SHUNT_VOLT  0x01
#define REG_BUS_VOLT    0x02
#define REG_POWER       0x03
#define REG_CURRENT     0x04
#define REG_CALIBRATION 0x05

// Конфигурация по умолчанию:
// AVG=4 (001), VbusCT=1.1ms (010), VshCT=1.1ms (010), Mode=Shunt+Bus Continuous (111)
#define CONFIG_DEFAULT  0x4527

INA226::INA226(uint8_t addr) : _addr(addr), _wire(nullptr), _shuntOhm(0.1f), _currentLSB_mA(0) {}

bool INA226::begin(TwoWire& wire, float shunt_ohm) {
    _wire = &wire;
    _wire->begin();  // I2C master
    _shuntOhm = shunt_ohm;
    configure(shunt_ohm);

    // Проверка: читаем CONFIG и сравниваем с записанным
    uint16_t val = readRegister(REG_CONFIG);
    return (val == CONFIG_DEFAULT);
}

void INA226::configure(float shunt_ohm) {
    _shuntOhm = shunt_ohm;

    // Записываем конфигурацию
    writeRegister(REG_CONFIG, CONFIG_DEFAULT);

    // Калибровка тока:
    // currentLSB = shuntVoltageLSB / shunt_ohm = 2.5e-6 / shunt_ohm  (А)
    // Для шунта 0.1 Ом: currentLSB = 25 мкА = 0.025 мА
    _currentLSB_mA = (SHUNT_LSB_uV / 1000.0f) / shunt_ohm;

    // Calibration register: 0.00512 / (currentLSB_А * shunt_ohm)
    // currentLSB_А = _currentLSB_mA / 1000
    // cal = 0.00512 / ((_currentLSB_mA / 1000) * shunt_ohm)
    float currentLSB_A = _currentLSB_mA / 1000.0f;
    uint16_t cal = (uint16_t)(0.00512f / (currentLSB_A * shunt_ohm));
    writeRegister(REG_CALIBRATION, cal);
}

float INA226::readBusVoltage() {
    uint16_t raw = readRegister(REG_BUS_VOLT);
    return (float)raw * BUS_LSB_mV;  // мВ
}

float INA226::readShuntVoltage() {
    int16_t raw = (int16_t)readRegister(REG_SHUNT_VOLT);
    return (float)raw * SHUNT_LSB_uV / 1000.0f;  // мВ
}

float INA226::readCurrent() {
    int16_t raw = (int16_t)readRegister(REG_CURRENT);
    return (float)raw * _currentLSB_mA;  // мА
}

uint16_t INA226::readRegister(uint8_t reg) {
    _wire->beginTransmission(_addr);
    _wire->write(reg);
    _wire->endTransmission(false);
    _wire->requestFrom(_addr, (uint8_t)2);
    if (_wire->available() < 2) return 0;
    uint16_t hi = _wire->read();
    uint16_t lo = _wire->read();
    return (hi << 8) | lo;
}

void INA226::writeRegister(uint8_t reg, uint16_t value) {
    _wire->beginTransmission(_addr);
    _wire->write(reg);
    _wire->write((uint8_t)(value >> 8));
    _wire->write((uint8_t)(value & 0xFF));
    _wire->endTransmission();
}
