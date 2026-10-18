//collects data from all sensors
#ifndef SENSORS_DATA_H
#define SENSORS_DATA_H

#include <WiFi.h>

extern bool debug_mode_active;

// Set while the RCU drives the shared TX line (HC-12 AT configuration) or while
// the OBC must not put free-form text on the air. Nothing may print when true.
bool obcRadioSilent = false;

// Why the last reset happened. Turns "it rebooted" into a fact: a servo or
// radio current spike that browns the board out reports BROWNOUT, a firmware
// fault reports PANIC, and they need completely different fixes.
#include <esp_system.h>
const char* resetReasonName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
  }
}

// Defined in msk_radio.h, which is included last. Free-form console text is
// only allowed on the wire in RADIO_OFF and RADIO_FULL modes - in STREAM mode
// the link carries nothing but MSK frames.
bool radioConsoleAllowed();

#define LOG_INFO(msg)  do { if (radioConsoleAllowed()) Serial.println("[INFO]  " + String(msg)); } while (0)
#define LOG_WARN(msg)  do { if (radioConsoleAllowed()) Serial.println("[WARN]  ▲ " + String(msg)); } while (0)
#define LOG_ERROR(msg) do { if (radioConsoleAllowed()) Serial.println("[ERROR] ▲ " + String(msg)); } while (0)

void logDebug(String message){
  if(debug_mode_active && radioConsoleAllowed()){
    Serial.print("[");
    Serial.print(millis());
    Serial.print(" ms] ");
    Serial.println(message);
  }
}

#define sensor_t adafruit_sensor_t
#include <Adafruit_Sensor.h>
#undef sensor_t
//#include "Adafruit_BME680.h"
#include "environment_sensor.h"
#include "position_sensor.h"
#include "ADC.h"
#include "RTC.h"
#include "camera.h"
#include <ArduinoJson.h>
#include "power_measure.h"

struct pointer_of_sensors{
  bme_struct * bme_;
  mpu * mpu_;
  ads_struct * ads_;
  ina_struct * ina_;
  rtc_struct * rtc_;
} pointers;

struct init_status_sensors{
  bool bme_;
  bool mpu_;
  bool ads_;
  bool ina_;
  bool rtc_;
  bool camera_;
} init_status;

void initSensors(){
  init_status.bme_ = initBME();
  init_status.mpu_ = initMPU();
  init_status.ads_ = initADS();
  init_status.ina_ = initINA();
  init_status.rtc_ = initRTC();
  init_status.camera_ = init_camera();
}

pointer_of_sensors * get_sensors_data(){
  logDebug("---Reading Sensors START---");
  if (init_status.bme_){
    unsigned long t = millis();
    pointers.bme_ = get_bme_data(); 
    unsigned long dt = millis() - t;
    if (debug_mode_active) logDebug("BME680 Read: " + String(dt) + " ms");
  }
  if (init_status.mpu_){
    unsigned long t = millis();
    pointers.mpu_ = get_mpu_data();
    unsigned long dt = millis() - t;
    if (debug_mode_active) logDebug("MPU**** Read: " + String(dt) + " ms");
  }
  if (init_status.ads_){
    unsigned long t = millis();
    pointers.ads_ = get_ads_data();
    unsigned long dt = millis() - t;
    if (debug_mode_active) logDebug("ADS1015 Read: " + String(dt) + " ms");
  }
  if(init_status.ina_){
    unsigned long t = millis();
    pointers.ina_ = get_ina_data();
    unsigned long dt = millis() - t;
    if (debug_mode_active) logDebug("INA3221 Read: " + String(dt) + " ms");
  }
  if (init_status.rtc_){
      pointers.rtc_ = get_rtc();
  }
  logDebug("---Reading Sensors END---");
  return &pointers;
}

#endif
