/**
 * Minimal self-contained BME280 driver — shares the panel's I2C bus.
 *
 * The Waveshare panel library brings up ESP-IDF's *legacy* I2C driver on
 * port 0 (GPIO 8 = SDA, 9 = SCL) for the touch controller and IO expander.
 * I2C is multi-drop, so a BME280 (address 0x76/0x77) coexists on that same bus
 * with the touch (0x5D) and expander (0x24). This driver issues transactions
 * to the sensor through that already-installed legacy driver — no second bus,
 * no Arduino Wire (which would pull in the conflicting new driver and abort).
 *
 * So: wire the sensor to the board's I2C connector (the touch bus) and call
 * begin() after the panel has started.
 */
#pragma once

#include <Arduino.h>

class Bme280 {
public:
    /** Probe 0x76 then 0x77 on the given I2C port (default 0 = touch bus).
     *  The port must already be installed by the panel. */
    bool begin(int i2c_port = 0);

    bool ok() const { return m_ok; }

    /** tC = deg C, rh = %RH, hPa = pressure. False on an I2C error. */
    bool read(float &tC, float &rh, float &hPa);

    uint8_t address() const { return m_addr; }

private:
    int      m_port = 0;
    uint8_t  m_addr = 0;
    bool     m_ok   = false;
    int32_t  m_tfine = 0;

    uint16_t T1; int16_t T2, T3;
    uint16_t P1; int16_t P2, P3, P4, P5, P6, P7, P8, P9;
    uint8_t  H1, H3; int16_t H2, H4, H5; int8_t H6;

    bool     probe(uint8_t addr);
    void     read_calibration();
    bool     rd(uint8_t reg, uint8_t *buf, uint8_t len);
    uint8_t  r8(uint8_t reg);
    uint16_t r16le(uint8_t reg);
    bool     w8(uint8_t reg, uint8_t val);
};
