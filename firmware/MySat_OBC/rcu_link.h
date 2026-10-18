/*
 * rcu_link.h - OBC-side driver for the RCU (ATmega328P radio controller).
 *
 * Replaces the stock control.h "Wire.write(byte(0))" one-liners with a checked
 * transport that can also read state back. Every stock behaviour is preserved:
 * setStateMotor()/control_motor() still emit the legacy single-byte commands,
 * so this firmware talks to an un-upgraded Nano too (it just cannot read
 * status from one - see rcuPresent()).
 */
#ifndef RCU_LINK_H
#define RCU_LINK_H

#include <Wire.h>
#include "msk_rcu_proto.h"
#include "sensors_data.h"   // LOG_* macros, logDebug()

// The OBC must not print while the RCU drives the shared TX line.
extern bool obcRadioSilent;

class RcuLink {
 public:
  bool begin() {
    present_ = probe();
    if (present_) {
      refresh();
      LOG_INFO("[RCU] Radio controller found, fw v" + String(status_.fw_major) +
               "." + String(status_.fw_minor) + " proto " + String(status_.proto));
      legacy_ = (status_.magic != RCU_STATUS_MAGIC);
    } else {
      LOG_WARN("[RCU] No radio controller on I2C 0x08 - radio commands disabled");
    }
    return present_;
  }

  bool present() const { return present_; }
  bool legacy()  const { return legacy_; }
  const RcuStatus& status() const { return status_; }

  // ---- legacy path (bit-compatible with stock MySat firmware) --------------
  bool sendLegacy(uint8_t cmd) {
    Wire.beginTransmission(RCU_I2C_ADDR);
    Wire.write(cmd);
    return Wire.endTransmission() == 0;
  }

  // ---- extended path ------------------------------------------------------
  bool send(uint8_t cmd) { return send(cmd, nullptr, 0); }

  bool send(uint8_t cmd, uint8_t a0) { return send(cmd, &a0, 1); }

  bool send(uint8_t cmd, uint8_t a0, uint8_t a1) {
    uint8_t args[2] = { a0, a1 };
    return send(cmd, args, 2);
  }

  bool send(uint8_t cmd, const uint8_t* args, uint8_t n) {
    if (!present_) return false;
    if (n > RCU_MAX_PAYLOAD) return false;

    uint8_t len = (uint8_t)(n + 2);       // cmd + args + xor
    uint8_t x = len ^ cmd;
    for (uint8_t i = 0; i < n; i++) x ^= args[i];

    Wire.beginTransmission(RCU_I2C_ADDR);
    Wire.write(RCU_FRAME_SOF);
    Wire.write(len);
    Wire.write(cmd);
    for (uint8_t i = 0; i < n; i++) Wire.write(args[i]);
    Wire.write(x);
    bool ok = (Wire.endTransmission() == 0);
    if (!ok) {
      failures_++;
      logDebug("[RCU] I2C write failed for cmd 0x" + String(cmd, HEX));
    }
    return ok;
  }

  // Pulls the status block. Returns false if the RCU did not answer with a
  // valid block (an un-upgraded stock Nano never will).
  bool refresh() {
    if (!present_) return false;
    uint8_t buf[RCU_STATUS_LEN];
    uint8_t got = Wire.requestFrom((uint8_t)RCU_I2C_ADDR, RCU_STATUS_LEN);
    if (got != RCU_STATUS_LEN) { legacy_ = true; return false; }
    for (uint8_t i = 0; i < RCU_STATUS_LEN && Wire.available(); i++) buf[i] = Wire.read();
    while (Wire.available()) Wire.read();

    if (buf[0] != RCU_STATUS_MAGIC) { legacy_ = true; return false; }
    memcpy(&status_, buf, RCU_STATUS_LEN);
    legacy_ = false;
    lastRefresh_ = millis();
    return true;
  }

  void poll(unsigned long intervalMs = 2000) {
    if (!present_ || legacy_) return;
    if (millis() - lastRefresh_ >= intervalMs) refresh();
  }

  // ---- radio helpers ------------------------------------------------------
  bool rfPower(bool on)          { return send(RCU_CMD_RF_POWER, on ? 1 : 0); }
  bool rfReset()                 { return send(RCU_CMD_RF_RESET); }
  bool rfSleep(bool sleep)       { return send(RCU_CMD_RF_SLEEP, sleep ? 1 : 0); }
  bool rfAtMode(bool on)         { return send(RCU_CMD_RF_AT_MODE, on ? 1 : 0); }
  bool armEmergency(bool on)     { return send(RCU_CMD_EMERGENCY_ARM, on ? 1 : 0); }
  bool kickWatchdog()            { return send(RCU_CMD_WDT_KICK); }
  bool setWatchdog(uint16_t s)   { return send(RCU_CMD_WDT_SET, (uint8_t)(s & 0xFF), (uint8_t)(s >> 8)); }
  bool setWatchdogAction(uint8_t a) { return send(RCU_CMD_WDT_ACTION, a); }
  bool servoAngle(uint8_t deg)   { return send(RCU_CMD_SERVO_ANGLE, deg); }

  bool setCallsign(const String& cs) {
    uint8_t args[6];
    for (uint8_t i = 0; i < 6; i++) args[i] = (i < cs.length()) ? (uint8_t)cs[i] : (uint8_t)' ';
    return send(RCU_CMD_SET_CALLSIGN, args, 6);
  }

  // Reconfiguring the HC-12 needs the shared TX line. The RCU cannot take it
  // while the OBC is printing, so this brackets the change with a bus grant
  // and blocks (briefly) until the AT engine reports idle.
  bool configureRadio(uint8_t cmd, uint8_t value, unsigned long timeoutMs = 4000) {
    if (!present_ || legacy_) return false;

    obcRadioSilent = true;                 // stop all console/telemetry output
    delay(30);                             // let the last byte clock out
    bool ok = send(RCU_CMD_BUS_GRANT) && send(cmd, value) && send(RCU_CMD_RF_APPLY);

    unsigned long t0 = millis();
    while (ok && millis() - t0 < timeoutMs) {
      delay(120);
      if (refresh() && status_.at_state == RCU_AT_IDLE && status_.bus_owner == 0) break;
    }
    if (ok && millis() - t0 >= timeoutMs) ok = false;

    send(RCU_CMD_BUS_RELEASE);
    delay(20);
    obcRadioSilent = false;

    refresh();
    if (!ok) LOG_WARN("[RCU] Radio reconfiguration timed out");
    return ok && status_.last_error == RCU_ERR_NONE;
  }

  bool setChannel(uint8_t ch)   { return (ch >= 1 && ch <= 100) && configureRadio(RCU_CMD_RF_CHANNEL, ch); }
  bool setTxPower(uint8_t p)    { return (p >= 1 && p <= 8)     && configureRadio(RCU_CMD_RF_TXPOWER, p); }
  bool setFuMode(uint8_t m)     { return (m >= 1 && m <= 4)     && configureRadio(RCU_CMD_RF_FU_MODE, m); }
  bool setBaudIndex(uint8_t i)  { return (i < RCU_BAUD_COUNT)   && configureRadio(RCU_CMD_RF_BAUD, i); }

  String describe() const {
    if (!present_) return "absent";
    if (legacy_)   return "legacy (stock Nano firmware, status unavailable)";
    String s = "ch " + String(status_.rf_channel);
    s += " pwr " + String(status_.rf_txpower);
    s += " FU" + String(status_.rf_fu_mode);
    s += " @" + String(RCU_BAUD_TABLE[status_.rf_baud_idx < RCU_BAUD_COUNT ? status_.rf_baud_idx : RCU_BAUD_DEFAULT]);
    s += status_.rf_state == RCU_RF_ON ? " [on]" : (status_.rf_state == RCU_RF_SLEEPING ? " [sleep]" : " [off]");
    s += " rx=" + String(status_.rx_bytes) + "B/" + String(status_.rx_frames_ok) + "f";
    if (status_.last_error != RCU_ERR_NONE) s += " err=" + String(status_.last_error);
    return s;
  }

  unsigned long i2cFailures() const { return failures_; }

 private:
  bool probe() {
    Wire.beginTransmission(RCU_I2C_ADDR);
    return Wire.endTransmission() == 0;
  }

  RcuStatus status_ = {};
  bool present_ = false;
  bool legacy_ = false;
  unsigned long lastRefresh_ = 0;
  unsigned long failures_ = 0;
};

RcuLink rcu;

#endif  // RCU_LINK_H
