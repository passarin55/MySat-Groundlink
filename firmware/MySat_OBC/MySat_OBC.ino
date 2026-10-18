/*
 * MYSAT GROUNDLINK - OBC FIRMWARE
 * for ESP32-CAM   ! select "AI Thinker ESP32-CAM" in the Arduino IDE Boards Manager
 *
 * On-board computer firmware for the MySat 1U CubeSat trainer. Derived from the
 * MySatKit v1.4 reference firmware (github.com/MySatKit/MySatKit-Firmware,
 * and extended with:
 *
 *   - a real BME680 driver (the reference build shipped a stub because BSEC
 *     would not link) with an open IAQ approximation;
 *   - MSK-1, a CRC-protected ASCII frame protocol for the HC-12 radio link,
 *     carrying beacons, telemetry, events, acks and chunked image downlink;
 *   - telecommand uplink so the satellite can be operated entirely over the
 *     radio, not just from the USB console or the Wi-Fi Web GUI;
 *   - a driver for the MySat_RCU radio-controller firmware on the ATmega328P,
 *     which owns the HC-12 (power, AT configuration, channel, TX power) and
 *     acts as an independent link watchdog.
 *
 * Every stock console command and the whole Wi-Fi Web GUI still work unchanged.
 *
 * Bus note: on MySat boards the ESP32 UART0, the ATmega328P UART and the HC-12
 * share one TX/RX pair. Only one device may drive TX at a time - see the bus
 * grant handshake in rcu_link.h / MySat_RCU.
 *
 * version: v.2.0-GL
 * license: GPL-3.0-or-later (inherited from MySatKit-Firmware; see LICENSE)
 */

#include <Wire.h>
#include <LittleFS.h>
#include <Preferences.h>
#include "web_server.h"
#include "console.h"
#include "msk_radio.h"

String ssid = "";
String password = "";
String useWiFi = "";

bool debug_mode_active = false;

Preferences prefs;
bool isSystemStable = false;
unsigned long lastHeartbeatUpdate = 0;

bool loadWiFiConfig(String& ssid, String& password, String& useWiFi);
void saveWiFiConfig(const String& ssid, const String& password, const String& useWiFi);
void promptUserForWiFi(String& ssid, String& password);
void tryConnectWiFi();
void setWiFi();

const int def_SDA(15);
const int def_SCL(13);

void setup() {
  loadStateMotor();               // read the stored position only
  Serial.begin(115200);

  if (!LittleFS.begin(true)) {
    LOG_ERROR("[FS] Failed to mount LittleFS!");
    return;
  }
  LOG_INFO("[FS] LittleFS mounted successfully.");

  Wire.begin(def_SDA, def_SCL);
  initSensors();
  initStarLed();
  initSignalLed();
  setTime();
  initEventLog();
  loadLoggerState();
  loadCallSign(callSign);

  // The photo index used to be loaded only by initServer(), which meant the
  // camera ring buffer was invisible with Wi-Fi disabled. The radio needs it
  // either way, and the call is now idempotent.
  initPhotoStorage();

  // Brings up the RCU link and puts the first frame on the air.
  radioInit();

  if (loadWiFiConfig(ssid, password, useWiFi)) {
    if (useWiFi.equalsIgnoreCase("Yes")) {
      tryConnectWiFi();
      initServer();
    }
  }

  if (radioConsoleAllowed()) {
    Serial.println("\nType 'Help' for the command list, 'RadioStatus' for the link.");
  }
}

unsigned long lastSensorUpdate = 0;
const unsigned long SENSOR_INTERVAL = 500;

void loop() {
  if (useWiFi.equalsIgnoreCase("Yes")) {
    server.handleClient();
  }

  // Single owner of the UART: splits MSK frames from human console lines.
  radioSerialTask();

  checkSystemState();
  updateBlinkStarLed();

  unsigned long now = millis();
  if (now - lastSensorUpdate >= SENSOR_INTERVAL) {
    lastSensorUpdate = now;
    finalizeSystemStartup();            // one-shot stable-boot bookkeeping
    pointer_of_sensors* data = get_sensors_data();
    outputData(data);                   // console text / plotter + CSV logger
    radioTask(data);                    // beacon, telemetry, image downlink
  }

  // Deferred panel restore - see restorePanelPosition() for why this is not
  // done in setup().
  if (!motorRestored && millis() > 6000) restorePanelPosition();

  updateSystemHeartbeat();
  saveBsecState();                      // hourly gas-baseline persistence
}

bool loadWiFiConfig(String& ssid, String& password, String& useWiFi) {
  if(!LittleFS.exists("/config.txt")) return false;

  File file = LittleFS.open("/config.txt", "r");
  if (!file) {
    LOG_WARN("[CONFIG] No config.txt found");
    pauseToRead();
    return false;
  }

  int lineCount = 0;
  while (file.available()) {
    file.readStringUntil('\n');
    lineCount++;
  }
  file.close();

  if (lineCount < 3) {
    LOG_WARN("[CONFIG] Invalid config.txt: less than 3 lines");
    pauseToRead();
    LittleFS.remove("/config.txt");
    return false;
  }

  file = LittleFS.open("/config.txt", "r");
  useWiFi = file.readStringUntil('\n');
  useWiFi.trim();
  ssid = file.readStringUntil('\n');
  ssid.trim();
  password = file.readStringUntil('\n');
  password.trim();
  file.close();

  if (useWiFi.length() == 0 || ssid.length() == 0 || password.length() == 0) {
    LOG_WARN("[CONFIG] Invalid config.txt: empty fields");
    pauseToRead();
    return false;
  }

  return true;
}

void saveWiFiConfig(const String& ssid, const String& password, const String& useWiFi) {
  File file = LittleFS.open("/config.txt", "w");
  if (!file) {
    LOG_ERROR("[CONFIG] Can't write config.txt");
    pauseToRead();
    return;
  }
  file.println(useWiFi);
  file.println(ssid);
  file.println(password);
  file.close();
}

void promptUserForWiFi(String& ssid, String& password) {
  unsigned long lastRepeat = millis();

  Serial.print("Please enter WiFi ssid: ");
  while (ssid.length() == 0) {
    if (Serial.available()) {
      ssid = Serial.readStringUntil('\n');
      ssid.trim();
      Serial.println(ssid);
      break;
    }
    if (millis() - lastRepeat >= 10000) {
      Serial.print("\nPlease enter WiFi ssid: ");
      lastRepeat = millis();
    }
    delay(50);
  }

  lastRepeat = millis();

  Serial.print("Please enter WiFi password: ");
  while (password.length() == 0) {
    if (Serial.available()) {
      password = Serial.readStringUntil('\n');
      password.trim();
      Serial.println(password);
      break;
    }
    if (millis() - lastRepeat >= 10000) {
      Serial.print("\nPlease enter WiFi password: ");
      lastRepeat = millis();
    }
    delay(50);
  }
}

void tryConnectWiFi() {
  while (true) {
    Serial.print("Connecting to WiFi: ");
    Serial.println(ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());
    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 20) {
      delay(500);
      Serial.print(".");
      retry++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      LOG_INFO("[WIFI] Connected successfully!");
      pauseToRead();
      return;
    } else {
      LOG_WARN("[WIFI] Failed to connect to: " + ssid);
      pauseToRead();
      setWiFi();
      if (!useWiFi.equalsIgnoreCase("Yes")) {
        return;
      }
    }
  }
}

void setWiFi() {
  unsigned long lastRepeat = millis();

  Serial.println("Do you want to use WiFi? Yes/No");
  while (!Serial.available()) {
    if (millis() - lastRepeat >= 5000) {
      Serial.println("Do you want to use WiFi? Yes/No");
      lastRepeat = millis();
    }
    delay(50);
  }
  useWiFi = Serial.readStringUntil('\n');
  useWiFi.trim();

  if (useWiFi.equalsIgnoreCase("Yes")) {
    ssid = "";
    password = "";
    promptUserForWiFi(ssid, password);

    if (ssid.length() > 0 && password.length() > 0) {
      saveWiFiConfig(ssid, password, useWiFi);
    } else {
      LOG_WARN("[WIFI] SSID or password is empty. Config not saved.");
      pauseToRead();
    }
  } else {
    saveWiFiConfig("none", "none", useWiFi);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    LOG_INFO("[WIFI] WiFi disabled by user.");
    pauseToRead();
  }
}
