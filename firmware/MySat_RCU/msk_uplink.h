/*
 * msk_uplink.h - passive MSK-1 receiver for the RCU.
 *
 * The RCU's RX pin sits on the net driven by the HC-12's TXD (and by the USB
 * programmer), so it hears every uplink the ground station sends without any
 * help from the OBC. That is the whole point of putting a second processor on
 * the radio: if the ESP32 hangs, the ground can still reach the spacecraft.
 *
 * Frames addressed to "RCU" are executed here. Everything else is only
 * counted, so the OBC stays the normal command path.
 *
 * Fixed buffers throughout - the ATmega328P has 2 KB of RAM and no business
 * running the Arduino String class in an interrupt-adjacent hot path.
 */
#ifndef MSK_UPLINK_H
#define MSK_UPLINK_H

#include <Arduino.h>
#include "hc12.h"

#define MSKU_BUF        120
#define MSKU_MAX_FIELDS 8
#define MSKU_FIELD_LEN  16

// Implemented in the sketch.
void servoGoTo(uint8_t angle);
void rcuBeacon();
extern uint8_t  rcuCallsign[7];
extern uint16_t groundTcCount;
extern bool     emergencyArmed;
extern bool     obcPresumedDead;
extern uint16_t wdtTimeoutS;
extern unsigned long lastKickMs;

// ---------------------------------------------------------------------------
static uint16_t mskuCrc16(const char* d, uint8_t len) {
  uint16_t crc = 0xFFFF;
  for (uint8_t i = 0; i < len; i++) {
    crc ^= (uint16_t)((uint8_t)d[i]) << 8;
    for (uint8_t b = 0; b < 8; b++) {
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

// Emits a frame body as "$<body>*<crc>\n". Silently drops it when the RCU does
// not own the bus - transmitting then would collide with the OBC.
void mskuEmit(const char* body) {
  if (!busHeld()) return;
  uint16_t crc = mskuCrc16(body, (uint8_t)strlen(body));
  char tail[8];
  snprintf(tail, sizeof(tail), "*%04X\n", crc);
  Serial.print('$');
  Serial.print(body);
  Serial.print(tail);
  Serial.flush();
}

// The RCU may answer the ground when it holds a grant, or when the OBC has
// missed its watchdog and emergency commanding is armed.
static bool mskuMayTalk() {
  if (busHeld()) return true;
  if (emergencyArmed && obcPresumedDead) { busTake(); return true; }
  return false;
}

static void mskuAck(long refSeq, const char* status, const char* text) {
  bool tookBus = !busHeld();
  if (!mskuMayTalk()) return;
  char body[MSKU_BUF];
  snprintf(body, sizeof(body), "AK,0,%ld,%s,RCU %s", refSeq, status, text);
  mskuEmit(body);
  if (tookBus && !(emergencyArmed && obcPresumedDead)) busRelease();
}

// ---------------------------------------------------------------------------
// Incremental frame parser
// ---------------------------------------------------------------------------
struct MskuRx {
  char    buf[MSKU_BUF];
  uint8_t len     = 0;
  bool    active  = false;
  char    f[MSKU_MAX_FIELDS][MSKU_FIELD_LEN];
  uint8_t nFields = 0;
  uint16_t ok     = 0;
  uint16_t bad    = 0;
} rx;

static void mskuSplit() {
  rx.nFields = 0;
  uint8_t fi = 0, ci = 0;
  for (uint8_t i = 0; i < rx.len && fi < MSKU_MAX_FIELDS; i++) {
    char c = rx.buf[i];
    if (c == ',') {
      rx.f[fi][ci] = '\0';
      fi++;
      ci = 0;
    } else if (ci < MSKU_FIELD_LEN - 1) {
      rx.f[fi][ci++] = c;
    }
  }
  if (fi < MSKU_MAX_FIELDS) {
    rx.f[fi][ci] = '\0';
    fi++;
  }
  rx.nFields = fi;
}

static bool fieldIs(uint8_t i, const char* s) {
  return i < rx.nFields && strcasecmp(rx.f[i], s) == 0;
}

// $TC,<seq>,<dest>,<CMD>[,<arg>...]
// fields after split: 0="TC" 1=seq 2=dest 3=cmd 4=arg1 5=arg2
static void mskuHandleTc() {
  groundTcCount++;

  bool forUs = fieldIs(2, "RCU");
  if (!forUs) return;                 // the OBC owns every other address

  long seq = atol(rx.f[1]);

  if (!emergencyArmed) {
    mskuAck(seq, "DENY", "emergency commanding disarmed");
    return;
  }

  const char* cmd = (rx.nFields > 3) ? rx.f[3] : "";
  const char* a1  = (rx.nFields > 4) ? rx.f[4] : "";

  if (strcasecmp(cmd, "PING") == 0) {
    char msg[40];
    snprintf(msg, sizeof(msg), "alive resets=%u", (unsigned)rf.resets);
    mskuAck(seq, "OK", msg);

  } else if (strcasecmp(cmd, "RESET") == 0) {
    mskuAck(seq, "OK", "power-cycling HC-12");
    delay(60);
    rfHardReset();

  } else if (strcasecmp(cmd, "BEACON") == 0) {
    rcuBeacon();

  } else if (strcasecmp(cmd, "SOLAR") == 0) {
    if (strcasecmp(a1, "DEPLOY") == 0 || strcasecmp(a1, "OPEN") == 0) {
      servoGoTo(10);
      mskuAck(seq, "OK", "deploying");
    } else if (strcasecmp(a1, "RETRACT") == 0 || strcasecmp(a1, "CLOSE") == 0) {
      servoGoTo(170);
      mskuAck(seq, "OK", "retracting");
    } else {
      mskuAck(seq, "ERR", "DEPLOY|RETRACT");
    }

  } else if (strcasecmp(cmd, "RFCH") == 0) {
    int ch = atoi(a1);
    if (!mskuMayTalk()) return;                 // needs the bus to run AT
    if (rfQueueChannel((uint8_t)ch)) mskuAck(seq, "OK", "channel queued");
    else                             mskuAck(seq, "ERR", "1..100");

  } else if (strcasecmp(cmd, "WDTOFF") == 0) {
    wdtTimeoutS = 0;
    mskuAck(seq, "OK", "watchdog disabled");

  } else if (strcasecmp(cmd, "KICK") == 0) {
    lastKickMs = millis();
    obcPresumedDead = false;
    mskuAck(seq, "OK", "watchdog kicked");

  } else {
    mskuAck(seq, "UNK", "PING RESET BEACON SOLAR RFCH KICK WDTOFF");
  }
}

// Feed every received byte here.
void mskuFeed(char c) {
  if (c == '$') { rx.len = 0; rx.active = true; return; }
  if (!rx.active) return;

  if (c == '\n' || c == '\r') {
    rx.active = false;
    if (rx.len < 8) { return; }

    // ...*XXXX
    if (rx.buf[rx.len - 5] != '*') { rx.bad++; return; }
    rx.buf[rx.len] = '\0';
    char hex[5] = { rx.buf[rx.len - 4], rx.buf[rx.len - 3],
                    rx.buf[rx.len - 2], rx.buf[rx.len - 1], '\0' };
    uint16_t want = (uint16_t)strtoul(hex, NULL, 16);
    uint8_t bodyLen = (uint8_t)(rx.len - 5);
    if (mskuCrc16(rx.buf, bodyLen) != want) { rx.bad++; return; }

    rx.ok++;
    rx.len = bodyLen;
    rx.buf[rx.len] = '\0';
    mskuSplit();

    if (rx.nFields > 0 && strcasecmp(rx.f[0], "TC") == 0) mskuHandleTc();
    return;
  }

  if (rx.len >= MSKU_BUF - 2) { rx.active = false; rx.bad++; return; }
  rx.buf[rx.len++] = c;
}

#endif  // MSK_UPLINK_H
