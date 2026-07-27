/**
 * BME280 driver — shares the panel's legacy I2C bus (port 0). See bme280.h.
 *
 * Compensation reference: Bosch BME280 datasheet rev 1.6, section 8.1.
 */
#include "bme280.h"
#include "driver/i2c.h"

#define REG_CHIPID    0xD0
#define REG_CTRL_HUM  0xF2
#define REG_CTRL_MEAS 0xF4
#define REG_CONFIG    0xF5
#define REG_DATA      0xF7
#define CHIP_BME280   0x60
#define I2C_TMO       pdMS_TO_TICKS(50)

/* Register read via the already-installed legacy master driver. */
bool Bme280::rd(uint8_t reg, uint8_t *buf, uint8_t len)
{
    return i2c_master_write_read_device((i2c_port_t)m_port, m_addr,
                                        &reg, 1, buf, len, I2C_TMO) == ESP_OK;
}

uint8_t Bme280::r8(uint8_t reg)
{
    uint8_t v = 0;
    rd(reg, &v, 1);
    return v;
}

uint16_t Bme280::r16le(uint8_t reg)
{
    uint8_t b[2] = {0, 0};
    rd(reg, b, 2);
    return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}

bool Bme280::w8(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = {reg, val};
    return i2c_master_write_to_device((i2c_port_t)m_port, m_addr,
                                      b, 2, I2C_TMO) == ESP_OK;
}

bool Bme280::probe(uint8_t addr)
{
    m_addr = addr;
    uint8_t id = 0;
    if (!rd(REG_CHIPID, &id, 1)) return false;
    return id == CHIP_BME280;
}

void Bme280::read_calibration()
{
    T1 = r16le(0x88); T2 = (int16_t)r16le(0x8A); T3 = (int16_t)r16le(0x8C);
    P1 = r16le(0x8E);
    P2 = (int16_t)r16le(0x90); P3 = (int16_t)r16le(0x92);
    P4 = (int16_t)r16le(0x94); P5 = (int16_t)r16le(0x96);
    P6 = (int16_t)r16le(0x98); P7 = (int16_t)r16le(0x9A);
    P8 = (int16_t)r16le(0x9C); P9 = (int16_t)r16le(0x9E);

    H1 = r8(0xA1);
    H2 = (int16_t)r16le(0xE1);
    H3 = r8(0xE3);
    uint8_t e4 = r8(0xE4), e5 = r8(0xE5), e6 = r8(0xE6);
    H4 = (int16_t)((e4 << 4) | (e5 & 0x0F));
    H5 = (int16_t)((e6 << 4) | (e5 >> 4));
    H6 = (int8_t)r8(0xE7);
}

bool Bme280::begin(int i2c_port)
{
    m_port = i2c_port;
    m_ok = false;

    if (!probe(0x76) && !probe(0x77)) return false;

    read_calibration();

    w8(REG_CTRL_HUM, 0x01);                           /* osrs_h x1 */
    w8(REG_CTRL_MEAS, (0x1 << 5) | (0x1 << 2) | 0x3); /* osrs_t/p x1, normal */
    w8(REG_CONFIG, 0x08);                             /* IIR filter x4 */

    m_ok = true;
    return true;
}

bool Bme280::read(float &tC, float &rh, float &hPa)
{
    if (!m_ok) return false;

    uint8_t d[8];
    if (!rd(REG_DATA, d, 8)) return false;

    int32_t adc_P = ((int32_t)d[0] << 12) | ((int32_t)d[1] << 4) | (d[2] >> 4);
    int32_t adc_T = ((int32_t)d[3] << 12) | ((int32_t)d[4] << 4) | (d[5] >> 4);
    int32_t adc_H = ((int32_t)d[6] << 8) | d[7];

    double v1 = (((double)adc_T) / 16384.0 - ((double)T1) / 1024.0) * (double)T2;
    double v2 = ((((double)adc_T) / 131072.0 - ((double)T1) / 8192.0) *
                 (((double)adc_T) / 131072.0 - ((double)T1) / 8192.0)) * (double)T3;
    m_tfine = (int32_t)(v1 + v2);
    tC = (float)((v1 + v2) / 5120.0);

    v1 = ((double)m_tfine / 2.0) - 64000.0;
    v2 = v1 * v1 * (double)P6 / 32768.0;
    v2 = v2 + v1 * (double)P5 * 2.0;
    v2 = (v2 / 4.0) + ((double)P4 * 65536.0);
    v1 = ((double)P3 * v1 * v1 / 524288.0 + (double)P2 * v1) / 524288.0;
    v1 = (1.0 + v1 / 32768.0) * (double)P1;
    if (v1 == 0.0) {
        hPa = 0;
    } else {
        double p = 1048576.0 - (double)adc_P;
        p = (p - (v2 / 4096.0)) * 6250.0 / v1;
        v1 = (double)P9 * p * p / 2147483648.0;
        v2 = p * (double)P8 / 32768.0;
        p = p + (v1 + v2 + (double)P7) / 16.0;
        hPa = (float)(p / 100.0);
    }

    double h = ((double)m_tfine) - 76800.0;
    h = ((double)adc_H - ((double)H4 * 64.0 + (double)H5 / 16384.0 * h)) *
        ((double)H2 / 65536.0 * (1.0 + (double)H6 / 67108864.0 * h *
         (1.0 + (double)H3 / 67108864.0 * h)));
    h = h * (1.0 - (double)H1 * h / 524288.0);
    if (h > 100.0) h = 100.0;
    else if (h < 0.0) h = 0.0;
    rh = (float)h;

    return true;
}
