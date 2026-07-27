/**
 * BME280 environment dashboard.
 *
 * Live tiles for temperature / humidity / pressure, a rolling temperature
 * trend, and two derived agronomy values — dew point and VPD (vapour-pressure
 * deficit) — that matter more than raw RH for greenhouses and microclimate.
 *
 * The sensor is read from an LVGL timer (I2C is fast), so there is no loop()
 * hook to wire up. If no sensor is present it shows a NO SENSOR state and keeps
 * probing, so it comes alive the moment you connect one.
 */
#pragma once

void bme_dash_create(void);
