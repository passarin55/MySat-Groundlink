/*
 * MYSAT GROUNDLINK - RCU (Radio Control Unit)

 * for the Nano board (ATmega328P)   ! select "Arduino Nano" in the Arduino IDE
 *
 * Replaces MySat_Nano_ATmega328p.ino. The stock sketch declared RF_TURN and
 * RF_SET in its command enum but left 
both cases empty - the HC-12 was never
 * actually driven by anything. This firmware makes the second microcontroller
 * a real radio controller:
 *
 *   - owns the HC-12: power, AT mode, channel, TX power, FU mode, sleep,
 *     automatic baud discovery, and hard power-cycle recovery;
 *   - arbitrates the TX line it shares with the ESP32, so the two never drive
 *     the same net at once (the stock sketch printed a banner straight into
 *     the ESP32's transmissions at boot);
 *   - listens to the ground uplink independently and can execute emergency
 *     telecommands addressed to "RCU" when the OBC has stopped kicking its
 *     watchdog;
 *   - still drives the solar-panel servo exactly as before, including the
 *     2.2 s power limit that keeps it from cooking itself.
 *
 * I2C slave 0x08. The legacy single-byte commands (0=open, 1=close, 2=rf turn,
 * 3=rf set) are still accepted, so stock ESP32 firmware keeps working.
 *
 * version: v.2.0-GL
 * license: GPL-3.0-or-later (inherited from MySatKit-Firmware; see LICENSE)
 */

#define FW_MAJOR 2
#define FW_MINOR 0

#include <Wire.h>
#include <Servo.h>
#include <EEPROM.h>

// The Arduino preprocessor inserts generated prototypes just before the first
// function definition in the sketch. Some of those prototypes take an I2cCmd&,
// so the type has to be visible by then.
struct I2cCmd;
#include "msk_rcu_proto.h"
#include "hc12.h"
#include "msk_uplink.h"

// ---------------------------------------------------------------------------
// Servo (solar panel deployment)
// ---------------------------------------------------------------------------
const uint8_t MOTOR_PIN         = 9;
const int     POWER_SUPPLY_MS   = 2200;   // hard limit against overheating
const int     STEP_DELAY_MS     = 10;

uint8_t openedAngle = 10;
uint8_t closedAngle = 170;

Servo    mysatServo;
uint8_t  servoState  = RCU_SERVO_OFF;
uint8_t  servoAngle  = 170;               // last known position, restored below
uint8_t  servoTarget = 170;

// The believed position has to survive a reset, and specifically has to survive
// a brownout reset. If the RCU comes back thinking the panels are stowed while
// they are actually deployed, the next move commands a huge step and the servo
// draws its stall current - which browns the board out again.
#define EE_MAGIC_ADDR 0
#define EE_ANGLE_ADDR 1
#define EE_MAGIC      0xA7

void loadServoAngle() {
  if (EEPROM.read(EE_MAGIC_ADDR) == EE_MAGIC) {
    uint8_t a = EEPROM.read(EE_ANGLE_ADDR);
    if (a <= 180) { servoAngle = a; servoTarget = a; }
  }
}

void saveServoAngle() {
  EEPROM.update(EE_MAGIC_ADDR, EE_MAGIC);   // update() skips identical writes
  EEPROM.update(EE_ANGLE_ADDR, servoAngle);
}

// ---------------------------------------------------------------------------
// Shared state (referenced from msk_uplink.h)
// ---------------------------------------------------------------------------
uint8_t  rcuCallsign[7]  = { 'M', 'Y', 'S', 'A', 'T', ' ', '\0' };
uint16_t groundTcCount   = 0;
bool     emergencyArmed  = false;
bool     obcPresumedDead = false;
uint16_t wdtTimeoutS     = 0;
uint8_t  wdtAction       = RCU_WDT_BOTH;
unsigned long lastKickMs = 0;

// Saturating count of every byte that arrives on D0. If this stays at zero the
// RCU's RX pin is not connected to the serial bus at all, which is the one
// thing that decides whether independent ground commanding can work.
uint16_t rcuRxBytes = 0;
#define rxByteCount rcuRxBytes

// ---------------------------------------------------------------------------
// I2C command ring (filled in the ISR, drained in loop())
// ---------------------------------------------------------------------------
#define I2C_RING 6
struct I2cCmd {
  uint8_t cmd;
  uint8_t arg[RCU_MAX_PAYLOAD];
  uint8_t nArgs;
};
volatile I2cCmd i2cRing[I2C_RING];
volatile uint8_t i2cHead = 0, i2cTail = 0;
volatile uint8_t lastError = RCU_ERR_NONE;

RcuStatus statusBlock;

// ---------------------------------------------------------------------------
void servoGoTo(uint8_t angle) {
  if (angle > 180) angle = 180;
  if (servoState != RCU_SERVO_OFF) return;   // one move at a time
  servoTarget = angle;
  if (servoTarget == servoAngle) return;
  servoState = RCU_SERVO_NEW;
}

void servoTask() {
  static unsigned long powerTimer;
  static unsigned long stepTimer;

  switch (servoState) {
    case RCU_SERVO_NEW:
      // Servo::attach() starts emitting the library's default 1500 us pulse,
      // which is mid-travel - roughly 90 degrees. With the panels at 10 or 170
      // that is an 80 degree error the instant the output goes live, so the
      // servo pulls its stall current before the gentle 1 deg/10 ms ramp below
      // ever begins. Measured on this board that spike browns the OBC out
      // (reset_reason = BROWNOUT) even at 4.2 V on the charger.
      //
      // Writing the current position immediately after attach makes the first
      // pulse a hold command instead of a slam. write() before attach() is a
      // no-op in the Servo library, so it has to be this order.
      mysatServo.attach(MOTOR_PIN);
      mysatServo.write(servoAngle);
      powerTimer = millis();
      stepTimer  = millis();
      servoState = RCU_SERVO_TURNING;
      break;

    case RCU_SERVO_TURNING:
      if (millis() - powerTimer >= (unsigned long)POWER_SUPPLY_MS) {
        servoState = RCU_SERVO_DONE;          // power budget spent
        break;
      }
      if (millis() - stepTimer <= (unsigned long)STEP_DELAY_MS) break;
      if (servoAngle == servoTarget) {
        servoState = RCU_SERVO_DONE;
        break;
      }
      servoAngle += (servoTarget > servoAngle) ? 1 : -1;
      mysatServo.write(servoAngle);
      stepTimer = millis();
      break;

    case RCU_SERVO_DONE:
      mysatServo.detach();
      saveServoAngle();
      servoState = RCU_SERVO_OFF;
      break;

    case RCU_SERVO_OFF:
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Beacon of last resort. Only ever transmitted while the RCU legitimately
// owns the bus, i.e. after the OBC has missed its watchdog.
// ---------------------------------------------------------------------------
void rcuBeacon() {
  bool tookBus = false;
  if (!busHeld()) {
    if (!(emergencyArmed && obcPresumedDead)) return;
    busTake();
    tookBus = true;
    delay(2);
  }

  char body[MSKU_BUF];
  snprintf(body, sizeof(body),
           "BC,0,%s,%lu,9,0.00,%u,0",
           (const char*)rcuCallsign,
           (unsigned long)(millis() / 1000UL),
           (unsigned)((rf.power == RCU_RF_ON ? 1 : 0) | (obcPresumedDead ? 2 : 0)));
  mskuEmit(body);

  char ev[MSKU_BUF];
  snprintf(ev, sizeof(ev), "EV,0,%s,WARN,OBC watchdog expired - RCU is holding the link",
           (const char*)rcuCallsign);
  mskuEmit(ev);

  if (tookBus && !obcPresumedDead) busRelease();
}

// ---------------------------------------------------------------------------
// I2C
// ---------------------------------------------------------------------------
// Decodes one I2C write into `c`. Returns false if the frame was malformed.
static bool decodeI2cFrame(int howMany, I2cCmd& c) {
  if (howMany <= 0) return false;

  uint8_t first = (uint8_t)Wire.read();
  howMany--;
  c.nArgs = 0;

  if (first != RCU_FRAME_SOF) {
    c.cmd = first;                     // legacy single byte
    return true;
  }

  // [0xA5][len][cmd][args...][xor]
  if (howMany < 2) return false;
  uint8_t len = (uint8_t)Wire.read();
  howMany--;
  if (len < 2 || len > RCU_MAX_PAYLOAD + 2) return false;

  uint8_t body[RCU_MAX_PAYLOAD + 2];
  uint8_t n = 0;
  while (howMany > 0 && n < len) {
    body[n++] = (uint8_t)Wire.read();
    howMany--;
  }
  if (n != len) return false;

  uint8_t x = len;
  for (uint8_t i = 0; i < n - 1; i++) x ^= body[i];
  if (x != body[n - 1]) return false;

  c.cmd = body[0];
  c.nArgs = (uint8_t)(n - 2);
  for (uint8_t i = 0; i < c.nArgs; i++) c.arg[i] = body[i + 1];
  return true;
}

void onI2cReceive(int howMany) {
  // A zero-length write is how an I2C master probes for a slave
  // (beginTransmission/endTransmission with no payload). The OBC does this at
  // boot to detect us; it is not a malformed frame.
  if (howMany <= 0) return;

  I2cCmd c;
  if (decodeI2cFrame(howMany, c)) {
    uint8_t next = (uint8_t)((i2cHead + 1) % I2C_RING);
    if (next != i2cTail) {
      i2cRing[i2cHead].cmd = c.cmd;
      i2cRing[i2cHead].nArgs = c.nArgs;
      for (uint8_t i = 0; i < RCU_MAX_PAYLOAD; i++) i2cRing[i2cHead].arg[i] = c.arg[i];
      i2cHead = next;
    }
  } else {
    lastError = RCU_ERR_BAD_CRC;
  }
  while (Wire.available()) Wire.read();
}

void onI2cRequest() {
  statusBlock.magic           = RCU_STATUS_MAGIC;
  statusBlock.proto           = RCU_PROTO_VERSION;
  statusBlock.fw_major        = FW_MAJOR;
  statusBlock.fw_minor        = FW_MINOR;
  statusBlock.rf_state        = rf.power;
  statusBlock.at_state        = at.state;
  statusBlock.rf_channel      = rf.channel;
  statusBlock.rf_txpower      = rf.txpower;
  statusBlock.rf_baud_idx     = rf.baudIdx;
  statusBlock.rf_fu_mode      = rf.fuMode;
  statusBlock.servo_state     = servoState;
  statusBlock.servo_angle     = servoAngle;
  statusBlock.bus_owner       = busHeld() ? 1 : 0;
  statusBlock.emergency_armed = emergencyArmed ? 1 : 0;
  statusBlock.wdt_action      = wdtAction;
  statusBlock.last_error      = (at.lastError != RCU_ERR_NONE) ? at.lastError : lastError;
  statusBlock.wdt_timeout_s   = wdtTimeoutS;
  statusBlock.wdt_remaining_s = 0;
  if (wdtTimeoutS) {
    unsigned long elapsed = (millis() - lastKickMs) / 1000UL;
    statusBlock.wdt_remaining_s = (elapsed >= wdtTimeoutS) ? 0
                                : (uint16_t)(wdtTimeoutS - elapsed);
  }
  statusBlock.uptime_s        = millis() / 1000UL;
  statusBlock.rf_resets       = rf.resets;
  statusBlock.ground_tc_count = groundTcCount;
  statusBlock.rx_frames_ok    = (rx.ok  > 255) ? 255 : (uint8_t)rx.ok;
  statusBlock.rx_frames_bad   = (rx.bad > 255) ? 255 : (uint8_t)rx.bad;
  statusBlock.rx_bytes        = (rcuRxBytes > 255) ? 255 : (uint8_t)rcuRxBytes;

  Wire.write((const uint8_t*)&statusBlock, sizeof(statusBlock));
}

// ---------------------------------------------------------------------------
void execCommand(const I2cCmd& c) {
  switch (c.cmd) {

    // ---- legacy -----------------------------------------------------------
    case RCU_CMD_MOTOR_OPEN:  servoGoTo(openedAngle); break;
    case RCU_CMD_MOTOR_CLOSE: servoGoTo(closedAngle); break;
    case RCU_CMD_RF_TURN:     rfPowerSet(rf.power != RCU_RF_ON); break;
    case RCU_CMD_RF_SET:
      // Stock behaviour: park the HC-12 in AT mode so the operator can type AT
      // commands from the Serial Monitor. The RCU stays off the bus.
      rfSetPin(true);
      at.state = RCU_AT_IDLE;
      break;

    // ---- radio ------------------------------------------------------------
    case RCU_CMD_RF_POWER:
      if (c.nArgs < 1) { lastError = RCU_ERR_BAD_ARG; break; }
      rfPowerSet(c.arg[0] != 0);
      break;

    case RCU_CMD_RF_AT_MODE:
      if (c.nArgs < 1) { lastError = RCU_ERR_BAD_ARG; break; }
      rfSetPin(c.arg[0] != 0);
      break;

    case RCU_CMD_RF_CHANNEL:
      if (c.nArgs < 1 || !rfQueueChannel(c.arg[0])) lastError = RCU_ERR_BAD_ARG;
      break;

    case RCU_CMD_RF_TXPOWER:
      if (c.nArgs < 1 || !rfQueueTxPower(c.arg[0])) lastError = RCU_ERR_BAD_ARG;
      break;

    case RCU_CMD_RF_FU_MODE:
      if (c.nArgs < 1 || !rfQueueFuMode(c.arg[0])) lastError = RCU_ERR_BAD_ARG;
      break;

    case RCU_CMD_RF_BAUD:
      if (c.nArgs < 1 || !rfQueueBaud(c.arg[0])) lastError = RCU_ERR_BAD_ARG;
      break;

    case RCU_CMD_RF_SLEEP:
      if (c.nArgs < 1 || !rfQueueSleep(c.arg[0] != 0)) lastError = RCU_ERR_BAD_ARG;
      break;

    case RCU_CMD_RF_RESET:
      rfHardReset();
      break;

    case RCU_CMD_RF_SYNC:
      at.wantSync = true;
      at.syncIdx = 0;
      break;

    case RCU_CMD_RF_APPLY:
      if (!busHeld()) lastError = RCU_ERR_NO_BUS;
      break;                                   // atTask() drains the queue

    // ---- servo ------------------------------------------------------------
    case RCU_CMD_SERVO_ANGLE:
      if (c.nArgs < 1 || c.arg[0] > 180) { lastError = RCU_ERR_BAD_ARG; break; }
      servoGoTo(c.arg[0]);
      break;

    case RCU_CMD_SERVO_STOP:
      if (servoState == RCU_SERVO_TURNING) servoState = RCU_SERVO_DONE;
      break;

    case RCU_CMD_SERVO_LIMITS:
      if (c.nArgs < 2) { lastError = RCU_ERR_BAD_ARG; break; }
      openedAngle = c.arg[0];
      closedAngle = c.arg[1];
      break;

    // ---- watchdog ---------------------------------------------------------
    case RCU_CMD_WDT_SET:
      if (c.nArgs < 2) { lastError = RCU_ERR_BAD_ARG; break; }
      wdtTimeoutS = (uint16_t)c.arg[0] | ((uint16_t)c.arg[1] << 8);
      lastKickMs = millis();
      obcPresumedDead = false;
      break;

    case RCU_CMD_WDT_KICK:
      lastKickMs = millis();
      if (obcPresumedDead) {                   // the OBC is back
        obcPresumedDead = false;
        busRelease();
      }
      break;

    case RCU_CMD_WDT_ACTION:
      if (c.nArgs < 1 || c.arg[0] > RCU_WDT_BOTH) { lastError = RCU_ERR_BAD_ARG; break; }
      wdtAction = c.arg[0];
      break;

    // ---- bus --------------------------------------------------------------
    case RCU_CMD_BUS_GRANT:   busTake();    break;
    case RCU_CMD_BUS_RELEASE: busRelease(); break;

    case RCU_CMD_SET_CALLSIGN: {
      uint8_t n = (c.nArgs < 6) ? c.nArgs : 6;
      for (uint8_t i = 0; i < 6; i++) rcuCallsign[i] = (i < n) ? c.arg[i] : '\0';
      rcuCallsign[6] = '\0';
      for (int8_t i = 5; i >= 0 && rcuCallsign[i] == ' '; i--) rcuCallsign[i] = '\0';
      break;
    }

    // ---- misc -------------------------------------------------------------
    case RCU_CMD_EMERGENCY_ARM:
      if (c.nArgs < 1) { lastError = RCU_ERR_BAD_ARG; break; }
      emergencyArmed = (c.arg[0] != 0);
      break;

    case RCU_CMD_PING:
      lastKickMs = millis();
      break;

    case RCU_CMD_CLEAR_ERROR:
      lastError = RCU_ERR_NONE;
      at.lastError = RCU_ERR_NONE;
      break;

    default:
      lastError = RCU_ERR_BAD_CMD;
      break;
  }
}

void i2cTask() {
  while (i2cTail != i2cHead) {
    I2cCmd c;
    noInterrupts();
    c.cmd   = i2cRing[i2cTail].cmd;
    c.nArgs = i2cRing[i2cTail].nArgs;
    for (uint8_t i = 0; i < RCU_MAX_PAYLOAD; i++) c.arg[i] = i2cRing[i2cTail].arg[i];
    i2cTail = (uint8_t)((i2cTail + 1) % I2C_RING);
    interrupts();
    execCommand(c);
  }
}

// ---------------------------------------------------------------------------
// Watchdog: if the OBC stops kicking, the link is on its own.
// ---------------------------------------------------------------------------
void watchdogTask() {
  if (wdtTimeoutS == 0) return;
  if (obcPresumedDead) {
    static unsigned long lastBeacon = 0;
    if ((wdtAction & RCU_WDT_BEACON) && millis() - lastBeacon > 30000UL) {
      lastBeacon = millis();
      rcuBeacon();
    }
    return;
  }

  if ((millis() - lastKickMs) / 1000UL < wdtTimeoutS) return;

  obcPresumedDead = true;
  lastError = RCU_ERR_RF_DEAD;

  if (wdtAction & RCU_WDT_RF_RESET) {
    // The OBC is quiet, so taking the bus is safe. Recover a wedged HC-12
    // before trying to talk through it.
    busTake();
    rfHardReset();
    delay(200);
  }
  if (wdtAction & RCU_WDT_BEACON) {
    busTake();
    rcuBeacon();
  }
}

// ---------------------------------------------------------------------------
// Status LED. The pattern tells you what the RCU is doing without a console -
// which matters, because the RCU deliberately keeps off the serial bus.
//   slow single blink   : normal, transparent mode, OBC alive
//   double blink        : AT engine busy (configuring the HC-12)
//   fast continuous     : OBC watchdog expired, RCU holding the link
//   long on / short off : HC-12 powered down
// ---------------------------------------------------------------------------
void ledTask() {
  static unsigned long timer = 0;
  static uint8_t step = 0;

  const uint16_t* pattern;
  uint8_t steps;

  static const uint16_t pNormal[] = { 80, 2400 };
  static const uint16_t pAt[]     = { 80, 120, 80, 900 };
  static const uint16_t pDead[]   = { 100, 100 };
  static const uint16_t pRfOff[]  = { 1500, 200 };

  if (obcPresumedDead)            { pattern = pDead;   steps = 2; }
  else if (rf.power != RCU_RF_ON) { pattern = pRfOff;  steps = 2; }
  else if (atBusy())              { pattern = pAt;     steps = 4; }
  else                            { pattern = pNormal; steps = 2; }

  if (step >= steps) step = 0;
  if (millis() - timer > pattern[step]) {
    digitalWrite(LED_BUILTIN, (step % 2) == 0 ? HIGH : LOW);
    timer = millis();
    step = (uint8_t)((step + 1) % steps);
  }
}

// ---------------------------------------------------------------------------
void setup() {
  pinMode(LED_BUILTIN, OUTPUT);

  // The stock sketch wrote RF_SET_PIN LOW without ever making it an output,
  // which only enabled the internal pull-up. Both control pins are real
  // outputs here.
  pinMode(RF_ON_PIN, OUTPUT);
  pinMode(RF_SET_PIN, OUTPUT);
  rfPowerSet(true);        // D5 LOW  -> HC-12 powered (inverting P-MOSFET)
  rfSetPin(false);         // D4 LOW  -> SET released  -> transparent mode

  loadServoAngle();          // before any move is accepted

  Wire.begin(RCU_I2C_ADDR);
  Wire.onReceive(onI2cReceive);
  Wire.onRequest(onI2cRequest);

  Serial.begin(115200);
  busRelease();            // high-Z immediately: the ESP32 owns TX by default

  lastKickMs = millis();
}

void loop() {
  while (Serial.available()) {
    if (rcuRxBytes < 0xFFFF) rcuRxBytes++;
    mskuFeed((char)Serial.read());
  }

  i2cTask();
  atTask();
  servoTask();
  watchdogTask();
  ledTask();
}
