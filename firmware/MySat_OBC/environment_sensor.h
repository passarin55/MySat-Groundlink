/*
 * environment_sensor.h - BME680 driver (temperature, pressure, humidity, gas).
 *
 * The stock firmware shipped this as a stub returning constants because the
 * Bosch BSEC library is distributed as a pre-built blob that fails to link on
 * some ESP32 core versions. This is a direct register-level implementation of
 * the datasheet's floating-point compensation instead: no blob, no linker
 * trouble, real measurements.
 *
 * IAQ here is an open approximation (humidity + gas-baseline heuristic), NOT
 * Bosch's certified BSEC IAQ. It is good enough to see air quality trends and
 * is reported with an accuracy flag so the ground station knows how much to
 * trust it:
 *      0 = warming up      1 = baseline still settling
 *      2 = calibrated      3 = calibrated from a stored baseline
 */
#ifndef ENVIRONMENTSENSOR_H
#define ENVIRONMENTSENSOR_H

#include <Wire.h>
#include <Preferences.h>
#include <math.h>

#define BME680_I2C_ADDR      0x77
#define BME680_ALT_ADDR      0x76
#define BME680_CHIP_ID       0x61

// registers
#define BME_REG_CHIPID       0xD0
#define BME_REG_RESET        0xE0
#define BME_REG_CTRL_GAS0    0x70
#define BME_REG_CTRL_GAS1    0x71
#define BME_REG_CTRL_HUM     0x72
#define BME_REG_CTRL_MEAS    0x74
#define BME_REG_CONFIG       0x75
#define BME_REG_RES_HEAT0    0x5A
#define BME_REG_GAS_WAIT0    0x64
#define BME_REG_MEAS_STATUS  0x1D
#define BME_REG_FIELD0       0x1F   // press[3] temp[3] hum[2]

// IAQ tuning
#define IAQ_HUM_OPTIMAL      40.0f
#define IAQ_HUM_WEIGHT       0.25f
#define IAQ_BURN_IN_MS       300000UL   // 5 min before any IAQ is reported
#define IAQ_SETTLE_MS        1200000UL  // 20 min before accuracy 2
#define IAQ_BASELINE_KEY     "gasbase"

struct bme_struct {
  float temperature    = 0.0f;
  float humidity       = 0.0f;
  float gas_resistance = 0.0f;   // kOhm (console.h prints "KOhms")
  float pressure       = 0.0f;   // hPa
  float iaq            = 0.0f;   // 0 (excellent) .. 500 (severe)
  int   iaq_accuracy   = 0;
  bool  data_available = false;
  bool  first_save_done = false;
} bme_data;

Preferences preferences;

// ---------------------------------------------------------------------------
// Calibration coefficients
// ---------------------------------------------------------------------------
struct BmeCalib {
  uint16_t t1; int16_t t2; int8_t t3;
  uint16_t p1; int16_t p2; int8_t p3; int16_t p4, p5; int8_t p6, p7;
  int16_t p8, p9; uint8_t p10;
  uint16_t h1, h2; int8_t h3, h4, h5; uint8_t h6; int8_t h7;
  int8_t g1; int16_t g2; int8_t g3;
  uint8_t res_heat_range; int8_t res_heat_val; int8_t range_sw_err;
} bmeCal;

static uint8_t  bmeAddr = BME680_I2C_ADDR;
static float    bmeTFine = 0.0f;
static bool     bmeReady = false;
static unsigned long bmeBootMs = 0;
static unsigned long bmeLastKick = 0;
static unsigned long lastSaveTime = 0;
static float    gasBaseline = 0.0f;      // best (highest) clean-air resistance, ohm
static bool     gasBaselineFromNvs = false;
static uint8_t  bmeMeasPending = 0;

// ---------------------------------------------------------------------------
// Raw I2C helpers
// ---------------------------------------------------------------------------
static bool bmeWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(bmeAddr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool bmeRead(uint8_t reg, uint8_t* buf, uint8_t len) {
  Wire.beginTransmission(bmeAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(bmeAddr, len) != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

static uint8_t bmeRead8(uint8_t reg) {
  uint8_t v = 0;
  bmeRead(reg, &v, 1);
  return v;
}

static uint16_t bmeRead16LE(uint8_t reg) {
  uint8_t b[2];
  if (!bmeRead(reg, b, 2)) return 0;
  return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}

// ---------------------------------------------------------------------------
// Calibration readout (BME680 datasheet memory map)
// ---------------------------------------------------------------------------
static void bmeReadCalib() {
  bmeCal.t1 = bmeRead16LE(0xE9);
  bmeCal.t2 = (int16_t)bmeRead16LE(0x8A);
  bmeCal.t3 = (int8_t)bmeRead8(0x8C);

  bmeCal.p1 = bmeRead16LE(0x8E);
  bmeCal.p2 = (int16_t)bmeRead16LE(0x90);
  bmeCal.p3 = (int8_t)bmeRead8(0x92);
  bmeCal.p4 = (int16_t)bmeRead16LE(0x94);
  bmeCal.p5 = (int16_t)bmeRead16LE(0x96);
  bmeCal.p6 = (int8_t)bmeRead8(0x99);
  bmeCal.p7 = (int8_t)bmeRead8(0x98);
  bmeCal.p8 = (int16_t)bmeRead16LE(0x9C);
  bmeCal.p9 = (int16_t)bmeRead16LE(0x9E);
  bmeCal.p10 = bmeRead8(0xA0);

  uint8_t e1 = bmeRead8(0xE1), e2 = bmeRead8(0xE2), e3 = bmeRead8(0xE3);
  bmeCal.h1 = (uint16_t)(((uint16_t)e3 << 4) | (e2 & 0x0F));
  bmeCal.h2 = (uint16_t)(((uint16_t)e1 << 4) | (e2 >> 4));
  bmeCal.h3 = (int8_t)bmeRead8(0xE4);
  bmeCal.h4 = (int8_t)bmeRead8(0xE5);
  bmeCal.h5 = (int8_t)bmeRead8(0xE6);
  bmeCal.h6 = bmeRead8(0xE7);
  bmeCal.h7 = (int8_t)bmeRead8(0xE8);

  bmeCal.g1 = (int8_t)bmeRead8(0xED);
  bmeCal.g2 = (int16_t)bmeRead16LE(0xEB);
  bmeCal.g3 = (int8_t)bmeRead8(0xEE);

  bmeCal.res_heat_range = (uint8_t)((bmeRead8(0x02) >> 4) & 0x03);
  bmeCal.res_heat_val   = (int8_t)bmeRead8(0x00);
  bmeCal.range_sw_err   = (int8_t)(((int8_t)bmeRead8(0x04) & (int8_t)0xF0) / 16);
}

// ---------------------------------------------------------------------------
// Heater setpoint helpers
// ---------------------------------------------------------------------------
static uint8_t bmeCalcResHeat(uint16_t targetC, float ambientC) {
  if (targetC > 400) targetC = 400;
  float var1 = ((float)bmeCal.g1 / 16.0f) + 49.0f;
  float var2 = (((float)bmeCal.g2 / 32768.0f) * 0.0005f) + 0.00235f;
  float var3 = (float)bmeCal.g3 / 1024.0f;
  float var4 = var1 * (1.0f + (var2 * (float)targetC));
  float var5 = var4 + (var3 * ambientC);
  float res  = 3.4f * ((var5 * (4.0f / (4.0f + (float)bmeCal.res_heat_range)) *
                        (1.0f / (1.0f + ((float)bmeCal.res_heat_val * 0.002f)))) - 25.0f);
  if (res < 0) res = 0;
  if (res > 255) res = 255;
  return (uint8_t)res;
}

static uint8_t bmeCalcGasWait(uint16_t durMs) {
  if (durMs >= 0xFC0) return 0xFF;
  uint8_t factor = 0;
  while (durMs > 0x3F) { durMs /= 4; factor++; }
  return (uint8_t)(durMs + factor * 64);
}

// ---------------------------------------------------------------------------
// Compensation (datasheet floating-point reference implementation)
// ---------------------------------------------------------------------------
static float bmeCompTemp(uint32_t adc) {
  float v1 = ((float)adc / 16384.0f) - ((float)bmeCal.t1 / 1024.0f);
  v1 *= (float)bmeCal.t2;
  float v2 = ((float)adc / 131072.0f) - ((float)bmeCal.t1 / 8192.0f);
  v2 = v2 * v2 * (float)bmeCal.t3 * 16.0f;
  bmeTFine = v1 + v2;
  return bmeTFine / 5120.0f;
}

static float bmeCompPress(uint32_t adc) {
  float v1 = (bmeTFine / 2.0f) - 64000.0f;
  float v2 = v1 * v1 * ((float)bmeCal.p6 / 131072.0f);
  v2 += v1 * (float)bmeCal.p5 * 2.0f;
  v2 = (v2 / 4.0f) + ((float)bmeCal.p4 * 65536.0f);
  v1 = ((((float)bmeCal.p3 * v1 * v1) / 16384.0f) + ((float)bmeCal.p2 * v1)) / 524288.0f;
  v1 = (1.0f + (v1 / 32768.0f)) * (float)bmeCal.p1;
  if (v1 == 0.0f) return 0.0f;

  float p = 1048576.0f - (float)adc;
  p = ((p - (v2 / 4096.0f)) * 6250.0f) / v1;
  float v3 = ((float)bmeCal.p9 * p * p) / 2147483648.0f;
  float v4 = p * ((float)bmeCal.p8 / 32768.0f);
  float v5 = (p / 256.0f) * (p / 256.0f) * (p / 256.0f) * ((float)bmeCal.p10 / 131072.0f);
  p += (v3 + v4 + v5 + ((float)bmeCal.p7 * 128.0f)) / 16.0f;
  return p;   // Pa
}

static float bmeCompHum(uint16_t adc, float tempC) {
  float v1 = (float)adc - (((float)bmeCal.h1 * 16.0f) + (((float)bmeCal.h3 / 2.0f) * tempC));
  float v2 = v1 * (((float)bmeCal.h2 / 262144.0f) *
                   (1.0f + (((float)bmeCal.h4 / 16384.0f) * tempC) +
                           (((float)bmeCal.h5 / 1048576.0f) * tempC * tempC)));
  float v3 = (float)bmeCal.h6 / 16384.0f;
  float v4 = (float)bmeCal.h7 / 2097152.0f;
  float h = v2 + ((v3 + (v4 * tempC)) * v2 * v2);
  if (h > 100.0f) h = 100.0f;
  if (h < 0.0f)   h = 0.0f;
  return h;
}

static float bmeCompGas(uint16_t adc, uint8_t range) {
  static const float k1[16] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, -0.8f,
                                0.0f, 0.0f, -0.2f, -0.5f, 0.0f, -1.0f, 0.0f, 0.0f };
  static const float k2[16] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.1f, 0.7f, 0.0f, -0.8f,
                                -0.1f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
  if (range > 15) return 0.0f;
  float v1 = 1340.0f + (5.0f * (float)bmeCal.range_sw_err);
  float v2 = v1 * (1.0f + (k1[range] / 100.0f));
  float v3 = 1.0f + (k2[range] / 100.0f);
  float denom = v3 * 0.000000125f * (float)(1 << range) * ((((float)adc - 512.0f) / v2) + 1.0f);
  if (denom == 0.0f) return 0.0f;
  return 1.0f / denom;   // ohm
}

// ---------------------------------------------------------------------------
// IAQ heuristic
// ---------------------------------------------------------------------------
static void bmeUpdateIaq(float gasOhm, float humidity) {
  unsigned long age = millis() - bmeBootMs;

  // Track the highest resistance seen - that is our "clean air" reference.
  if (gasOhm > gasBaseline) gasBaseline = gasOhm;

  if (age < IAQ_BURN_IN_MS && !gasBaselineFromNvs) {
    bme_data.iaq = 0.0f;
    bme_data.iaq_accuracy = 0;
    return;
  }
  if (gasBaseline <= 0.0f) {
    bme_data.iaq_accuracy = 0;
    return;
  }

  // Humidity component: penalise deviation from ~40 %RH.
  float humScore;
  if (humidity >= 38.0f && humidity <= 42.0f) {
    humScore = IAQ_HUM_WEIGHT * 100.0f;
  } else if (humidity < 38.0f) {
    humScore = (humidity / IAQ_HUM_OPTIMAL) * IAQ_HUM_WEIGHT * 100.0f;
  } else {
    humScore = ((100.0f - humidity) / (100.0f - IAQ_HUM_OPTIMAL)) * IAQ_HUM_WEIGHT * 100.0f;
  }
  if (humScore < 0.0f) humScore = 0.0f;

  // Gas component: ratio against the clean-air baseline.
  float ratio = gasOhm / gasBaseline;
  if (ratio > 1.0f) ratio = 1.0f;
  if (ratio < 0.0f) ratio = 0.0f;
  float gasScore = ratio * (1.0f - IAQ_HUM_WEIGHT) * 100.0f;

  float quality = humScore + gasScore;          // 0 (bad) .. 100 (clean)
  bme_data.iaq = (100.0f - quality) * 5.0f;     // 0 (clean) .. 500 (severe)
  if (bme_data.iaq < 0.0f)   bme_data.iaq = 0.0f;
  if (bme_data.iaq > 500.0f) bme_data.iaq = 500.0f;

  if (gasBaselineFromNvs)              bme_data.iaq_accuracy = 3;
  else if (age >= IAQ_SETTLE_MS)       bme_data.iaq_accuracy = 2;
  else                                 bme_data.iaq_accuracy = 1;
}

// ---------------------------------------------------------------------------
// Baseline persistence (keeps the old saveBsecState() entry point)
// ---------------------------------------------------------------------------
void loadState(void* = nullptr) {
  preferences.begin("bme", true);
  float stored = preferences.getFloat(IAQ_BASELINE_KEY, 0.0f);
  preferences.end();
  if (stored > 1000.0f) {
    gasBaseline = stored;
    gasBaselineFromNvs = true;
    logDebug("[BME] Gas baseline restored: " + String(stored / 1000.0f, 1) + " kOhm");
  }
}

void saveState(void* = nullptr) {
  if (gasBaseline <= 1000.0f) return;
  preferences.begin("bme", false);
  preferences.putFloat(IAQ_BASELINE_KEY, gasBaseline);
  preferences.end();
  bme_data.first_save_done = true;
  logDebug("[BME] Gas baseline saved: " + String(gasBaseline / 1000.0f, 1) + " kOhm");
}

void saveBsecState() {
  unsigned long now = millis();
  if (now - lastSaveTime >= 3600000UL) {   // hourly, as in the stock firmware
    lastSaveTime = now;
    saveState();
  }
}

// ---------------------------------------------------------------------------
// Init / read
// ---------------------------------------------------------------------------
static void bmeStartMeasurement() {
  // ctrl_hum: osrs_h = 2x
  bmeWrite(BME_REG_CTRL_HUM, 0x02);
  // config: IIR filter coefficient 3
  bmeWrite(BME_REG_CONFIG, 0x02 << 2);
  // heater profile 0: 320 degC for 150 ms
  bmeWrite(BME_REG_RES_HEAT0, bmeCalcResHeat(320, bme_data.temperature > 0 ? bme_data.temperature : 25.0f));
  bmeWrite(BME_REG_GAS_WAIT0, bmeCalcGasWait(150));
  bmeWrite(BME_REG_CTRL_GAS0, 0x00);        // heater on
  bmeWrite(BME_REG_CTRL_GAS1, 0x10 | 0x00); // run_gas = 1, nb_conv = 0
  // ctrl_meas: osrs_t = 4x (0b100), osrs_p = 4x (0b100), mode = forced (0b01)
  bmeWrite(BME_REG_CTRL_MEAS, (0x04 << 5) | (0x04 << 2) | 0x01);
  bmeMeasPending = 1;
  bmeLastKick = millis();
}

bool initBME() {
  bmeAddr = BME680_I2C_ADDR;
  Wire.beginTransmission(bmeAddr);
  if (Wire.endTransmission() != 0) {
    bmeAddr = BME680_ALT_ADDR;                 // some boards strap SDO low
    Wire.beginTransmission(bmeAddr);
    if (Wire.endTransmission() != 0) {
      LOG_WARN("[BME] No BME680 at 0x77 or 0x76");
      return false;
    }
  }

  uint8_t id = bmeRead8(BME_REG_CHIPID);
  if (id != BME680_CHIP_ID) {
    LOG_WARN("[BME] Unexpected chip id 0x" + String(id, HEX) + " at 0x" + String(bmeAddr, HEX));
    return false;
  }

  bmeWrite(BME_REG_RESET, 0xB6);
  delay(10);
  bmeReadCalib();
  loadState();

  bmeBootMs = millis();
  bmeReady = true;
  bmeStartMeasurement();

  LOG_INFO("[BME] BME680 ready at 0x" + String(bmeAddr, HEX) +
           (gasBaselineFromNvs ? " (baseline restored)" : " (burn-in 5 min)"));
  return true;
}

bme_struct* get_bme_data() {
  if (!bmeReady) return &bme_data;

  // Forced mode: one conversion per request. Poll for new_data, and re-arm if
  // the chip did not answer in time instead of blocking the flight loop.
  uint8_t status = bmeRead8(BME_REG_MEAS_STATUS);
  bool newData    = (status & 0x80) != 0;
  bool measuring  = (status & 0x20) != 0;
  bool gasRunning = (status & 0x40) != 0;

  if (!newData) {
    if (!measuring && !gasRunning && millis() - bmeLastKick > 500) bmeStartMeasurement();
    return &bme_data;   // previous sample stays valid
  }

  uint8_t d[8];
  if (!bmeRead(BME_REG_FIELD0, d, 8)) return &bme_data;

  uint32_t pAdc = ((uint32_t)d[0] << 12) | ((uint32_t)d[1] << 4) | ((uint32_t)d[2] >> 4);
  uint32_t tAdc = ((uint32_t)d[3] << 12) | ((uint32_t)d[4] << 4) | ((uint32_t)d[5] >> 4);
  uint16_t hAdc = ((uint16_t)d[6] << 8) | (uint16_t)d[7];

  uint8_t g[2];
  bmeRead(0x2A, g, 2);
  uint16_t gAdc     = (uint16_t)(((uint16_t)g[0] << 2) | (g[1] >> 6));
  uint8_t  gRange   = g[1] & 0x0F;
  bool     gasValid = (g[1] & 0x20) != 0;
  bool     heatStab = (g[1] & 0x10) != 0;

  bme_data.temperature = bmeCompTemp(tAdc);
  bme_data.pressure    = bmeCompPress(pAdc) / 100.0f;          // Pa -> hPa
  bme_data.humidity    = bmeCompHum(hAdc, bme_data.temperature);

  if (gasValid && heatStab) {
    float ohm = bmeCompGas(gAdc, gRange);
    bme_data.gas_resistance = ohm / 1000.0f;                   // ohm -> kOhm
    bmeUpdateIaq(ohm, bme_data.humidity);
  }

  bme_data.data_available = true;
  bmeStartMeasurement();                                       // arm the next one
  return &bme_data;
}

#endif  // ENVIRONMENTSENSOR_H
