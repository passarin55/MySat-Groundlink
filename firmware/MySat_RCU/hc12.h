/*
 * hc12.h - HC-12 transceiver control and shared-bus arbitration for the RCU.
 *
 * Board wiring (MySat board B 1.5.8, see MySatKit-PCB Schematic_1.5.8.board-B):
 *
 *   D5 -> Q4 (IRLML6402, P-channel high-side switch)  ->  HC-12 VCC
 *         The gate drive is INVERTING: D5 LOW = HC-12 powered ON.
 *         The stock firmware relies on this ("allows HC-12 to always work
 *         by default").
 *
 *   D4 -> Q3 (IRLML6244, N-channel) -> net SET_HC-12 -> HC-12 SET pin
 *         Also INVERTING: D4 HIGH = SET pin pulled LOW = AT command mode.
 *                         D4 LOW  = SET pin high      = transparent mode.
 *
 *   D0/D1 -> the TX/RX pair shared with the ESP32 and the HC-12.
 *
 * Bus arbitration
 * ---------------
 * The Nano's TX (PD1) sits on the same net as the ESP32's TX. Two push-pull
 * drivers on one net is exactly the kind of contention that makes an HC-12
 * wedge mid-flight, so this firmware keeps PD1 in high-Z (USART transmitter
 * disabled, pin as input, no pull-up) and only enables it while it holds a bus
 * grant from the OBC. The receiver stays enabled at all times, which is what
 * lets the RCU listen to the ground uplink for free.
 */
#ifndef HC12_H
#define HC12_H

#include <Arduino.h>
#include "msk_rcu_proto.h"

#define RF_ON_PIN        5      // LOW = powered
#define RF_SET_PIN       4      // HIGH = AT mode
#define RF_POWER_ON      LOW
#define RF_POWER_OFF     HIGH
#define RF_SET_ASSERT    HIGH
#define RF_SET_RELEASE   LOW

#define AT_ENTER_MS      60     // HC-12 datasheet: >40 ms after SET goes low
#define AT_LEAVE_MS      90     // >80 ms after SET is released
#define AT_REPLY_MS      400
#define AT_QUEUE_LEN     4
#define AT_CMD_MAX       20
#define RF_RESET_OFF_MS  250

// ---------------------------------------------------------------------------
// Bus arbitration
// ---------------------------------------------------------------------------
static bool busOwned = false;

inline void busRelease() {
  if (busOwned) Serial.flush();          // let the last byte finish
  UCSR0B &= ~(1 << TXEN0);               // disable the USART transmitter
  pinMode(1, INPUT);                     // PD1 high-Z, no pull-up
  busOwned = false;
}

inline void busTake() {
  if (busOwned) return;
  UCSR0B |= (1 << TXEN0);                // the USART reclaims PD1
  busOwned = true;
}

inline bool busHeld() { return busOwned; }

// ---------------------------------------------------------------------------
// AT command engine
// ---------------------------------------------------------------------------
struct AtEngine {
  uint8_t  state       = RCU_AT_IDLE;
  char     queue[AT_QUEUE_LEN][AT_CMD_MAX];
  uint8_t  qHead       = 0;
  uint8_t  qCount      = 0;
  unsigned long timer  = 0;
  char     reply[24];
  uint8_t  replyLen    = 0;
  uint8_t  lastError   = RCU_ERR_NONE;
  bool     wantSync    = false;
  uint8_t  syncIdx     = 0;
} at;

struct RfState {
  uint8_t  power    = RCU_RF_ON;
  uint8_t  channel  = 1;
  uint8_t  txpower  = 8;
  uint8_t  baudIdx  = RCU_BAUD_DEFAULT;
  uint8_t  fuMode   = 3;
  uint16_t resets   = 0;
} rf;

inline void rfPowerSet(bool on) {
  digitalWrite(RF_ON_PIN, on ? RF_POWER_ON : RF_POWER_OFF);
  rf.power = on ? RCU_RF_ON : RCU_RF_OFF;
}

inline void rfSetPin(bool atMode) {
  digitalWrite(RF_SET_PIN, atMode ? RF_SET_ASSERT : RF_SET_RELEASE);
}

// Hard power-cycle. Recovers a module that has stopped responding - the
// documented failure the upstream v.1.4.1 release notes describe as
// "HC-12 stops working during operation".
void rfHardReset() {
  rfSetPin(false);
  rfPowerSet(false);
  delay(RF_RESET_OFF_MS);
  rfPowerSet(true);
  rf.resets++;
  at.state = RCU_AT_IDLE;
  at.qCount = 0;
  at.qHead = 0;
}

bool atEnqueue(const char* cmd) {
  if (at.qCount >= AT_QUEUE_LEN) return false;
  uint8_t slot = (uint8_t)((at.qHead + at.qCount) % AT_QUEUE_LEN);
  strncpy(at.queue[slot], cmd, AT_CMD_MAX - 1);
  at.queue[slot][AT_CMD_MAX - 1] = '\0';
  at.qCount++;
  return true;
}

static void atDrainRx() {
  while (Serial.available()) Serial.read();
}

// Non-blocking state machine. Must be called from loop().
void atTask() {
  switch (at.state) {

    case RCU_AT_IDLE:
      if (at.qCount == 0 && !at.wantSync) return;
      if (!busHeld()) {                       // cannot talk without the bus
        at.lastError = RCU_ERR_NO_BUS;
        at.qCount = 0;
        at.wantSync = false;
        return;
      }
      if (rf.power != RCU_RF_ON) rfPowerSet(true);
      rfSetPin(true);                         // enter AT mode
      atDrainRx();
      at.timer = millis();
      at.state = at.wantSync ? RCU_AT_SYNCING : RCU_AT_ENTERING;
      break;

    // -- automatic baud discovery ------------------------------------------
    // A factory HC-12 runs at 9600 while the MySat sketches run at 115200, so
    // AT commands would silently go nowhere. Sweep the table, find the rate
    // the module answers on, force it to 115200, then come back.
    case RCU_AT_SYNCING: {
      if (millis() - at.timer < AT_ENTER_MS) return;

      Serial.flush();
      Serial.begin(RCU_BAUD_TABLE[at.syncIdx]);
      busTake();
      delay(5);
      atDrainRx();
      Serial.print(F("AT\r\n"));
      Serial.flush();

      unsigned long t0 = millis();
      bool answered = false;
      uint8_t n = 0;
      char buf[8];
      while (millis() - t0 < 250) {
        if (Serial.available()) {
          char c = (char)Serial.read();
          if (n < sizeof(buf) - 1) buf[n++] = c;
          buf[n] = '\0';
          if (strstr(buf, "OK")) { answered = true; break; }
        }
      }

      if (answered) {
        Serial.print(F("AT+B115200\r\n"));
        Serial.flush();
        delay(120);
        Serial.begin(115200);
        busTake();
        rf.baudIdx = RCU_BAUD_DEFAULT;
        at.wantSync = false;
        at.syncIdx = 0;
        at.lastError = RCU_ERR_NONE;
        at.timer = millis();
        at.state = RCU_AT_LEAVING;
      } else if (++at.syncIdx >= RCU_BAUD_COUNT) {
        Serial.begin(115200);
        busTake();
        at.wantSync = false;
        at.syncIdx = 0;
        at.lastError = RCU_ERR_RF_DEAD;
        at.timer = millis();
        at.state = RCU_AT_LEAVING;
      }
      // else: loop again on the next baud
      break;
    }

    case RCU_AT_ENTERING:
      if (millis() - at.timer < AT_ENTER_MS) return;
      at.state = RCU_AT_SENDING;
      break;

    case RCU_AT_SENDING: {
      if (at.qCount == 0) {                   // nothing left - back out
        at.timer = millis();
        at.state = RCU_AT_LEAVING;
        return;
      }
      atDrainRx();
      Serial.print(at.queue[at.qHead]);
      Serial.print(F("\r\n"));
      Serial.flush();
      at.replyLen = 0;
      at.reply[0] = '\0';
      at.timer = millis();
      at.state = RCU_AT_WAITING;
      break;
    }

    case RCU_AT_WAITING: {
      while (Serial.available()) {
        char c = (char)Serial.read();
        if (at.replyLen < sizeof(at.reply) - 1) {
          at.reply[at.replyLen++] = c;
          at.reply[at.replyLen] = '\0';
        }
      }
      if (strstr(at.reply, "OK")) {
        at.qHead = (uint8_t)((at.qHead + 1) % AT_QUEUE_LEN);
        at.qCount--;
        at.lastError = RCU_ERR_NONE;
        at.state = RCU_AT_SENDING;
      } else if (millis() - at.timer > AT_REPLY_MS) {
        at.qHead = (uint8_t)((at.qHead + 1) % AT_QUEUE_LEN);
        at.qCount--;
        at.lastError = RCU_ERR_AT_TIMEOUT;
        at.state = (at.qCount > 0) ? RCU_AT_SENDING : RCU_AT_LEAVING;
        at.timer = millis();
      }
      break;
    }

    case RCU_AT_LEAVING:
      rfSetPin(false);                        // back to transparent mode
      if (millis() - at.timer < AT_LEAVE_MS) return;
      atDrainRx();
      at.state = RCU_AT_IDLE;
      break;

    case RCU_AT_ERROR:
    default:
      at.state = RCU_AT_IDLE;
      break;
  }
}

inline bool atBusy() { return at.state != RCU_AT_IDLE || at.qCount > 0 || at.wantSync; }

// ---------------------------------------------------------------------------
// Queue helpers for the individual HC-12 parameters.
// ---------------------------------------------------------------------------
bool rfQueueChannel(uint8_t ch) {
  if (ch < 1 || ch > 100) return false;
  char buf[AT_CMD_MAX];
  snprintf(buf, sizeof(buf), "AT+C%03u", (unsigned)ch);
  if (!atEnqueue(buf)) return false;
  rf.channel = ch;
  return true;
}

bool rfQueueTxPower(uint8_t p) {
  if (p < 1 || p > 8) return false;
  char buf[AT_CMD_MAX];
  snprintf(buf, sizeof(buf), "AT+P%u", (unsigned)p);
  if (!atEnqueue(buf)) return false;
  rf.txpower = p;
  return true;
}

bool rfQueueFuMode(uint8_t m) {
  if (m < 1 || m > 4) return false;
  char buf[AT_CMD_MAX];
  snprintf(buf, sizeof(buf), "AT+FU%u", (unsigned)m);
  if (!atEnqueue(buf)) return false;
  rf.fuMode = m;
  return true;
}

// Changing the serial rate breaks the link unless every node follows, so this
// only accepts the rate the sketches actually run at. Use RCU_CMD_RF_SYNC to
// bring a factory module (9600) up to 115200 automatically.
bool rfQueueBaud(uint8_t idx) {
  if (idx >= RCU_BAUD_COUNT) return false;
  if (idx != RCU_BAUD_DEFAULT) return false;
  char buf[AT_CMD_MAX];
  snprintf(buf, sizeof(buf), "AT+B%lu", (unsigned long)RCU_BAUD_TABLE[idx]);
  if (!atEnqueue(buf)) return false;
  rf.baudIdx = idx;
  return true;
}

bool rfQueueSleep(bool sleep) {
  if (sleep) {
    if (!atEnqueue("AT+SLEEP")) return false;
    rf.power = RCU_RF_SLEEPING;
  } else {
    if (!atEnqueue("AT")) return false;      // any AT wakes it
    rf.power = RCU_RF_ON;
  }
  return true;
}

#endif  // HC12_H
