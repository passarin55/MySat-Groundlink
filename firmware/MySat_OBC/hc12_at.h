/*
 * hc12_at.h - OBC-driven AT configuration of the HC-12.
 *
 * Why this exists
 * ---------------
 * The original design had the RCU (ATmega328P) run the AT session, because the
 * published board schematic's net labels put the Nano's D0/D1 on the same
 * TX/RX pair as the ESP32 and the HC-12. Measured on a real board 1.5.8, that
 * is not true:
 *
 *     rcu_rx_bytes      = 0     (the Nano's RX never sees a single byte)
 *     RFPWR -> last_error = 5   (RCU_ERR_AT_TIMEOUT: its TX never reaches the module)
 *
 * The ATmega328P's UART is simply not connected to that bus. What the RCU
 * *can* do is drive the HC-12's SET and power pins, because those are ordinary
 * GPIOs reachable over I2C.
 *
 * So the roles swap: the OBC talks AT to the module over its own UART0 - which
 * demonstrably reaches the HC-12, since that is how telemetry gets out - and
 * the RCU acts as a GPIO expander for SET. This works on the hardware as
 * built, with no extra wiring.
 *
 * The RCU-side AT engine is left in place; it becomes the correct path if you
 * ever add the two wires (HC-12 TXD -> Nano D0, HC-12 RXD -> Nano D1).
 */
#ifndef HC12_AT_H
#define HC12_AT_H

#include <Arduino.h>
#include "msk_rcu_proto.h"
#include "rcu_link.h"

#define HC12_SET_ENTER_MS   80     // datasheet: >40 ms after SET goes low
#define HC12_SET_LEAVE_MS   250    // datasheet says >80 ms after SET is released,
                                   // but the module needs noticeably longer than
                                   // that before it will actually carry RF again.
                                   // Acking too early sends the reply into a
                                   // transmitter that is not listening yet - the
                                   // cause of RFCFG timing out while PING never did.
#define HC12_AT_TIMEOUT_MS  600

struct AtResult {
  bool   ok = false;
  String reply = "";
};

// ---------------------------------------------------------------------------
// Raw AT exchange. Assumes SET is already asserted - use hc12Session() or the
// helpers below rather than calling this directly.
// ---------------------------------------------------------------------------
// `collectAll` keeps reading for the whole timeout instead of returning at the
// first "OK". AT+RX answers with four separate OK+ lines, so stopping early
// would report only the first of them.
static AtResult hc12Exchange(const String& cmd,
                             unsigned long timeoutMs = HC12_AT_TIMEOUT_MS,
                             bool collectAll = false) {
  AtResult res;

  while (Serial.available()) Serial.read();     // drop anything stale
  Serial.print(cmd);
  Serial.print("\r\n");
  Serial.flush();

  unsigned long t0 = millis();
  unsigned long lastByte = 0;
  String reply;
  reply.reserve(96);
  while (millis() - t0 < timeoutMs) {
    while (Serial.available()) {
      char c = (char)Serial.read();
      if (c == '\r') continue;
      if (c == '\n') c = ' ';                   // keep it to one frame field
      if (reply.length() < 120) reply += c;
      lastByte = millis();
    }
    if (reply.indexOf("OK") >= 0) {
      res.ok = true;
      if (!collectAll) break;
      // multi-line: stop once the module has been quiet for a moment
      if (lastByte && millis() - lastByte > 120) break;
    }
    delay(4);
  }
  reply.trim();
  res.reply = reply;
  return res;
}

// ---------------------------------------------------------------------------
// One complete AT session: mute the downlink, put the module in command mode,
// run `cmd`, come back to transparent mode.
//
// Muting matters. In command mode the HC-12 stops relaying RF entirely, so any
// frame emitted here would be lost, and worse, would be read back as if it
// were the module's reply.
// ---------------------------------------------------------------------------
AtResult hc12SendAt(const String& cmd, unsigned long timeoutMs = HC12_AT_TIMEOUT_MS,
                    bool collectAll = false) {
  AtResult res;
  if (!rcu.present()) {
    res.reply = "no radio controller - cannot reach the SET pin";
    return res;
  }

  obcRadioSilent = true;
  delay(25);                                    // let the last frame clock out

  rcu.send(RCU_CMD_RF_AT_MODE, 1);              // SET asserted -> command mode
  delay(HC12_SET_ENTER_MS);

  res = hc12Exchange(cmd, timeoutMs, collectAll);

  rcu.send(RCU_CMD_RF_AT_MODE, 0);              // back to transparent
  delay(HC12_SET_LEAVE_MS);
  while (Serial.available()) Serial.read();

  obcRadioSilent = false;
  return res;
}

// ---------------------------------------------------------------------------
// Parameter helpers. Each mirrors the RCU-side queue functions it replaces.
// ---------------------------------------------------------------------------
bool hc12SetChannel(uint8_t ch) {
  if (ch < 1 || ch > 100) return false;
  char buf[16];
  snprintf(buf, sizeof(buf), "AT+C%03u", (unsigned)ch);
  return hc12SendAt(buf).ok;
}

bool hc12SetTxPower(uint8_t p) {
  if (p < 1 || p > 8) return false;
  char buf[12];
  snprintf(buf, sizeof(buf), "AT+P%u", (unsigned)p);
  return hc12SendAt(buf).ok;
}

bool hc12SetFuMode(uint8_t m) {
  if (m < 1 || m > 4) return false;
  char buf[12];
  snprintf(buf, sizeof(buf), "AT+FU%u", (unsigned)m);
  return hc12SendAt(buf).ok;
}

// Reads the module's whole configuration back (AT+RX returns several lines).
String hc12ReadConfig() {
  AtResult r = hc12SendAt("AT+RX", 1200, true);   // multi-line
  return r.reply.length() ? r.reply : String("no reply");
}

// ---------------------------------------------------------------------------
// Automatic baud discovery.
//
// A factory HC-12 runs its serial port at 9600 while this firmware runs at
// 115200, so AT commands would silently go nowhere. Sweep the table with the
// ESP32's own UART, find the rate the module answers on, force it to 115200,
// and restore our port. SET is held for the whole sweep rather than toggled
// per attempt.
// ---------------------------------------------------------------------------
bool hc12Sync(String& report) {
  if (!rcu.present()) { report = "no radio controller"; return false; }

  obcRadioSilent = true;
  delay(25);
  rcu.send(RCU_CMD_RF_AT_MODE, 1);
  delay(HC12_SET_ENTER_MS);

  bool found = false;
  uint32_t foundAt = 0;
  for (uint8_t i = 0; i < RCU_BAUD_COUNT; i++) {
    Serial.updateBaudRate(RCU_BAUD_TABLE[i]);
    delay(30);
    if (hc12Exchange("AT", 260).ok) {
      found = true;
      foundAt = RCU_BAUD_TABLE[i];
      break;
    }
  }

  bool changed = false;
  if (found && foundAt != 115200UL) {
    changed = hc12Exchange("AT+B115200", 400).ok;
    delay(120);
  }

  Serial.updateBaudRate(115200);                // our port always ends at 115200
  delay(20);
  rcu.send(RCU_CMD_RF_AT_MODE, 0);
  delay(HC12_SET_LEAVE_MS);
  while (Serial.available()) Serial.read();
  obcRadioSilent = false;

  if (!found) {
    report = "no answer at any baud - check HC-12 power and the SET line";
    return false;
  }
  report = "module found at " + String(foundAt);
  if (changed)              report += ", switched to 115200";
  else if (foundAt == 115200UL) report += " (already correct)";
  else                      report += ", but AT+B115200 was refused";
  return true;
}

#endif  // HC12_AT_H
