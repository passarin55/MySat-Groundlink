/*
 * msk_radio.h - OBC radio service: beacon, telemetry downlink, telecommand
 *               uplink and image downlink over the shared UART / HC-12 link.
 *
 * Include LAST in the sketch: it needs callSign (console.h), stateLight
 * (web_server.h) and the sensor accessors.
 *
 * Bus model
 * ---------
 * ESP32 UART0, the RCU's UART and the HC-12 share one TX/RX pair, so:
 *   - anything printed here is transmitted over the air AND shown on USB;
 *   - anything the ground station sends is heard by the OBC and the RCU.
 * The RCU keeps its TX pin high-Z unless it holds a bus grant, so there is
 * exactly one driver on TX at any time.
 */
#ifndef MSK_RADIO_H
#define MSK_RADIO_H

#include <LittleFS.h>
#include "base64.h"
#include "msk_link.h"
#include "rcu_link.h"
#include "hc12_at.h"

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
#define RADIO_CFG_FILE        "/radio.cfg"
#define RADIO_IMG_CHUNK_RAW   132     // multiple of 3 -> 176 base64 chars
#define RADIO_IMG_MAX_PER_LOOP 1      // keep the main loop responsive

enum RadioMode {
  RADIO_OFF     = 0,   // no frames at all (stock console behaviour)
  RADIO_BEACON  = 1,   // periodic $BC only
  RADIO_STREAM  = 2,   // $BC + $TM, human console suppressed
  RADIO_FULL    = 3    // $BC + $TM + human console text interleaved
};

struct RadioConfig {
  RadioMode mode          = RADIO_BEACON;
  uint32_t  beacon_ms     = 10000;
  uint32_t  telemetry_ms  = 2000;
  uint16_t  img_pace_ms   = 25;      // gap between image chunks
  bool      require_dest  = true;    // reject TCs not addressed to us
  bool      ack_all       = true;
};

RadioConfig radioCfg;

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------
static MskParser  mskRx;
static MskParsed  mskMsg;
static uint32_t   txSeq        = 0;
static uint32_t   rxTcCount    = 0;
static uint32_t   rxTcRejected = 0;
static unsigned long lastBeacon = 0;
static unsigned long lastTmTx   = 0;
static String     consoleLine   = "";
static bool       rfSelfTestDone = false;
static unsigned long rfSelfTestAt = 0;   // millis() when the boot self-test runs

struct ImgDownlink {
  bool     active     = false;
  int      id         = 0;
  File     f;
  uint32_t bytes      = 0;
  uint32_t sent       = 0;
  uint16_t chunkIdx   = 0;
  uint16_t chunkCount = 0;
  uint32_t crc32      = 0;
  unsigned long lastChunk = 0;
} imgTx;

// Provided by console.h (legacy human console) - see the refactor there.
void handleConsoleLine(String line);
// Defined in the sketch; the WIFI telecommand persists credentials with it.
void saveWiFiConfig(const String& ssid, const String& password, const String& useWiFi);
void reactToCommand(String str);

// ---------------------------------------------------------------------------
// Low level output. Every downlink frame goes through here so a single flag
// can mute the transmitter while the RCU owns the bus.
// ---------------------------------------------------------------------------
static inline void radioEmit(const String& frame) {
  if (obcRadioSilent) return;
  Serial.print(frame);
}

static inline bool radioFramesEnabled() {
  return radioCfg.mode != RADIO_OFF;
}

// True when free-form console text is allowed on the wire.
bool radioConsoleAllowed() {
  if (obcRadioSilent) return false;
  return radioCfg.mode == RADIO_OFF || radioCfg.mode == RADIO_FULL;
}

// ---------------------------------------------------------------------------
// Frame emitters
// ---------------------------------------------------------------------------
void radioSendEvent(const char* level, const String& text) {
  if (!radioFramesEnabled()) return;
  MskFrame f(MSK_T_EVENT);
  f.add((unsigned long)++txSeq).add(callSign).add(level).add(text);
  radioEmit(f.finish());
}

void radioSendAck(long refSeq, const char* status, const String& text) {
  MskFrame f(MSK_T_ACK);
  f.add((unsigned long)++txSeq).add(refSeq).add(status).add(text);
  radioEmit(f.finish());
}

void radioSendBeacon(pointer_of_sensors* d) {
  float vbat = (init_status.ina_ && d) ? d->ina_->batteryVoltage : 0.0f;

  // Compact health word: bit0 BME, 1 MPU, 2 ADS, 3 INA, 4 RTC, 5 CAM,
  //                     6 solar deployed, 7 logging, 8 wifi, 9 RCU present.
  uint16_t flags = 0;
  flags |= init_status.bme_    ? (1 << 0) : 0;
  flags |= init_status.mpu_    ? (1 << 1) : 0;
  flags |= init_status.ads_    ? (1 << 2) : 0;
  flags |= init_status.ina_    ? (1 << 3) : 0;
  flags |= init_status.rtc_    ? (1 << 4) : 0;
  flags |= init_status.camera_ ? (1 << 5) : 0;
  flags |= stateMotor          ? (1 << 6) : 0;
  flags |= logger.enabled      ? (1 << 7) : 0;
  flags |= (WiFi.status() == WL_CONNECTED) ? (1 << 8) : 0;
  flags |= rcu.present()       ? (1 << 9) : 0;

  MskFrame f(MSK_T_BEACON);
  f.add((unsigned long)++txSeq)
   .add(callSign)
   .add((unsigned long)(millis() / 1000))
   .add((int)radioCfg.mode)
   .add(vbat, 2)
   .add((long)flags)
   .add((unsigned long)ESP.getFreeHeap());
  radioEmit(f.finish());
}

void radioSendTelemetry(pointer_of_sensors* d) {
  if (!radioFramesEnabled()) return;

  char utc[24] = "0000-00-00T00:00:00";
  if (init_status.rtc_) {
    snprintf(utc, sizeof(utc), "%04d-%02d-%02dT%02d:%02d:%02d",
             d->rtc_->year_, d->rtc_->month_, d->rtc_->day_,
             d->rtc_->hour_, d->rtc_->minute_, d->rtc_->second_);
  }

  MskFrame f(MSK_T_TELEMETRY);
  f.add((unsigned long)++txSeq)
   .add(callSign)
   .add((unsigned long)frameCounter)
   .add((unsigned long)(millis() / 1000))
   .add(utc);

  // environment
  if (init_status.bme_) {
    f.add(d->bme_->temperature, 2).add(d->bme_->humidity, 2)
     .add(d->bme_->pressure, 2).add(d->bme_->gas_resistance, 1)
     .add(d->bme_->iaq, 1).add((int)d->bme_->iaq_accuracy);
  } else {
    f.add("").add("").add("").add("").add("").add("");
  }

  // attitude
  if (init_status.mpu_ && calibration.valid) {
    f.add(d->mpu_->roll - offset_roll, 1)
     .add(d->mpu_->pitch - offset_pitch, 1)
     .add(d->mpu_->yaw - offset_yaw, 1);
  } else {
    f.add("").add("").add("");
  }

  // sun sensors
  if (init_status.ads_) {
    f.add(d->ads_->ph1, 0).add(d->ads_->ph2, 0)
     .add(d->ads_->ph3, 0).add(d->ads_->ph4, 0);
  } else {
    f.add("").add("").add("").add("");
  }

  // power
  if (init_status.ina_) {
    float il = d->ina_->leftSolarPanelCurrent;  if (il < 0) il = 0;
    float ir = d->ina_->rightSolarPanelCurrent; if (ir < 0) ir = 0;
    f.add(d->ina_->batteryVoltage, 3).add(d->ina_->batteryCurrent, 1)
     .add(d->ina_->SolarPanelVoltage, 3).add(il, 1).add(ir, 1);
  } else {
    f.add("").add("").add("").add("").add("");
  }

  // platform
  f.add(stateMotor)
   .add(stateLight)
   .add((int)getLoggingState())
   .add((unsigned long)ESP.getFreeHeap())
   .add((long)(LittleFS.totalBytes() - LittleFS.usedBytes()));

  radioEmit(f.finish());
}

void radioSendRadioStatus() {
  MskFrame f(MSK_T_RADIO);
  f.add((unsigned long)++txSeq).add(callSign);
  if (rcu.present() && !rcu.legacy()) {
    const RcuStatus& s = rcu.status();
    f.add((int)s.rf_state).add((int)s.at_state).add((int)s.rf_channel)
     .add((int)s.rf_txpower).add((long)RCU_BAUD_TABLE[s.rf_baud_idx < RCU_BAUD_COUNT ? s.rf_baud_idx : RCU_BAUD_DEFAULT])
     .add((int)s.rf_fu_mode).add((int)s.servo_angle)
     .add((long)s.wdt_remaining_s).add((int)s.last_error)
     .add((long)s.rf_resets).add((long)s.ground_tc_count)
     .add((long)s.rx_bytes).add((long)s.rx_frames_ok).add((long)s.rx_frames_bad);
  } else {
    f.add(-1).add(-1).add(-1).add(-1).add(-1).add(-1).add(-1);
    f.add(-1).add(-1).add(-1).add(-1).add(-1).add(-1).add(-1);
  }
  f.add((unsigned long)mskRx.framesOk()).add((unsigned long)mskRx.framesBad())
   .add((unsigned long)rxTcCount).add((unsigned long)rxTcRejected);
  radioEmit(f.finish());
}

// ---------------------------------------------------------------------------
// Image downlink
// ---------------------------------------------------------------------------
void radioImgAbort(const char* why) {
  if (!imgTx.active) return;
  if (imgTx.f) imgTx.f.close();
  imgTx.active = false;
  radioSendEvent("WARN", String("IMG_ABORT ") + why);
}

bool radioImgStart(int id) {
  if (imgTx.active) return false;

  PhotoRecord* rec = getPhotoById(id);
  if (!rec) return false;

  imgTx.f = LittleFS.open(rec->filename.c_str(), "r");
  if (!imgTx.f) return false;

  imgTx.active     = true;
  imgTx.id         = id;
  imgTx.bytes      = imgTx.f.size();
  imgTx.sent       = 0;
  imgTx.chunkIdx   = 0;
  imgTx.crc32      = 0;
  imgTx.chunkCount = (uint16_t)((imgTx.bytes + RADIO_IMG_CHUNK_RAW - 1) / RADIO_IMG_CHUNK_RAW);
  imgTx.lastChunk  = 0;

  MskFrame f(MSK_T_IMG_META);
  f.add((unsigned long)++txSeq).add(callSign).add((long)id)
   .add((long)imgTx.chunkCount).add((unsigned long)imgTx.bytes)
   .add((long)RADIO_IMG_CHUNK_RAW).add(rec->timestamp);
  radioEmit(f.finish());
  return true;
}

void radioImgTask() {
  if (!imgTx.active || obcRadioSilent) return;
  if (millis() - imgTx.lastChunk < radioCfg.img_pace_ms) return;

  for (uint8_t n = 0; n < RADIO_IMG_MAX_PER_LOOP; n++) {
    if (!imgTx.active) return;

    uint8_t raw[RADIO_IMG_CHUNK_RAW];
    int got = imgTx.f.read(raw, RADIO_IMG_CHUNK_RAW);
    if (got <= 0) {                       // finished
      MskFrame f(MSK_T_IMG_END);
      char crcbuf[12];
      snprintf(crcbuf, sizeof(crcbuf), "%08X", imgTx.crc32);
      f.add((unsigned long)++txSeq).add((long)imgTx.id)
       .add((long)imgTx.chunkIdx).add((unsigned long)imgTx.sent).add(crcbuf);
      radioEmit(f.finish());
      imgTx.f.close();
      imgTx.active = false;
      return;
    }

    imgTx.crc32 = mskCrc32Update(imgTx.crc32, raw, got);
    imgTx.sent += got;

    String b64 = base64::encode(raw, got);
    MskFrame f(MSK_T_IMG_DATA);
    f.add((unsigned long)++txSeq).add((long)imgTx.id).add((long)imgTx.chunkIdx).addRaw(b64);
    radioEmit(f.finish());

    imgTx.chunkIdx++;
    imgTx.lastChunk = millis();
  }
}

// Capture a fresh frame into the LittleFS ring buffer (same storage the Web
// GUI uses) so it can then be downlinked.
bool radioCapture(String& err) {
  if (!init_status.camera_) { err = "camera not initialised"; return false; }

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) { err = "capture failed"; return false; }

  char ts[24] = "0000-00-00 00:00:00";
  if (init_status.rtc_) {
    rtc_struct* t = get_rtc();
    snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d",
             t->year_, t->month_, t->day_, t->hour_, t->minute_, t->second_);
  }

  bool ok = savePhoto(fb, ts);
  esp_camera_fb_return(fb);
  if (!ok) err = "write failed";
  return ok;
}

// ---------------------------------------------------------------------------
// RF self-test.
//
// Asks the HC-12 "AT" and reports whether it answers. This exercises the one
// path nothing else can check on its own:
//
//     OBC TX -> HC-12 RXD   (known good whenever telemetry reaches the ground)
//     HC-12 TXD -> OBC RX   (the only way an uplink, or an AT reply, arrives)
//
// It runs automatically ~20 s after boot and reports over the downlink, so the
// answer arrives even when no uplink is available - which is precisely the
// situation you need it in. Beware that a USB-serial adapter attached to the
// programmer header also drives OBC RX and will mask a working module, so run
// this with the cable unplugged.
// ---------------------------------------------------------------------------
void radioRfSelfTest(long ackSeq = -1) {
  AtResult r = hc12SendAt("AT", 600);
  String msg;
  if (r.ok) {
    msg = "RF self-test PASS - HC-12 answered AT, TXD reaches the OBC";
  } else if (!rcu.present()) {
    msg = "RF self-test SKIPPED - no radio controller to toggle SET";
  } else {
    msg = "RF self-test FAIL - no AT reply";
    msg += r.reply.length() ? (" (saw \"" + r.reply + "\")")
                            : " (silence: HC-12 TXD may not reach the OBC RX,"
                              " or a USB adapter is holding that line)";
  }
  if (ackSeq >= 0) radioSendAck(ackSeq, r.ok ? MSK_ACK_OK : MSK_ACK_ERR, msg);
  radioSendEvent(r.ok ? "INFO" : "WARN", msg);
  writeEventLog(r.ok ? "RF_SELFTEST_PASS" : "RF_SELFTEST_FAIL");
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------
// A magic line so a truncated or corrupted file is recognised as such rather
// than being parsed into garbage. This matters more than it looks: the old
// loader called toInt() on whatever it found, and toInt() returns 0 for an
// unparseable line - which is exactly RADIO_OFF. A single interrupted write
// could therefore switch the radio off permanently, and the only way to turn
// it back on is a telecommand, which needs the radio. That happened on real
// hardware after a brownout loop corrupted the filesystem.
#define RADIO_CFG_MAGIC "MSKCFG1"

void radioSaveConfig() {
  File f = LittleFS.open(RADIO_CFG_FILE, "w");
  if (!f) return;
  f.println(RADIO_CFG_MAGIC);
  f.println((int)radioCfg.mode);
  f.println(radioCfg.beacon_ms);
  f.println(radioCfg.telemetry_ms);
  f.println(radioCfg.img_pace_ms);
  f.println(radioCfg.require_dest ? 1 : 0);
  f.close();
}

void radioLoadConfig() {
  if (!LittleFS.exists(RADIO_CFG_FILE)) return;
  File f = LittleFS.open(RADIO_CFG_FILE, "r");
  if (!f) return;

  String magic = f.readStringUntil('\n');
  magic.trim();
  if (magic != RADIO_CFG_MAGIC) {
    f.close();
    LittleFS.remove(RADIO_CFG_FILE);
    LOG_WARN("[RADIO] radio.cfg unreadable - reverting to defaults (BEACON)");
    return;                       // defaults stand; the radio is never silenced
  }

  int  mode = f.readStringUntil('\n').toInt();
  long b    = f.readStringUntil('\n').toInt();
  long t    = f.readStringUntil('\n').toInt();
  long p    = f.readStringUntil('\n').toInt();
  int  rd   = f.readStringUntil('\n').toInt();
  f.close();

  // RADIO_OFF is deliberately NOT restored. A spacecraft that comes back from
  // a reset with its transmitter disabled cannot be recovered from the ground,
  // so "off" lasts until the next reboot and no longer. Everything else
  // persists normally.
  if (mode > RADIO_OFF && mode <= RADIO_FULL) radioCfg.mode = (RadioMode)mode;
  else if (mode == RADIO_OFF) {
    radioCfg.mode = RADIO_BEACON;
    LOG_INFO("[RADIO] stored mode was OFF - coming up in BEACON so the link is reachable");
  }
  if (b >= 1000)  radioCfg.beacon_ms    = b;
  if (t >= 250)   radioCfg.telemetry_ms = t;
  if (p >= 0 && p <= 2000) radioCfg.img_pace_ms = p;
  radioCfg.require_dest = (rd != 0);
}

// ---------------------------------------------------------------------------
// Telecommand dispatch
// ---------------------------------------------------------------------------
// $TC,<seq>,<dest>,<CMD>[,<arg>...]*CRC
static void radioDispatchTc(const MskParsed& m) {
  long   seq  = m.asLong(0);
  String dest = m.at(1);
  String cmd  = m.at(2);
  cmd.toUpperCase();

  rxTcCount++;

  if (radioCfg.require_dest && dest != callSign && dest != "*") {
    rxTcRejected++;
    return;   // addressed to somebody else - stay silent, do not jam the band
  }

  String a1 = m.at(3), a2 = m.at(4), a3 = m.at(5);

  // ---- link / housekeeping ------------------------------------------------
  if (cmd == "PING") {
    radioSendAck(seq, MSK_ACK_OK, String("PONG ") + FIRMWARE_VERSION);

  } else if (cmd == "TM") {
    pointer_of_sensors* d = get_sensors_data();
    radioSendAck(seq, MSK_ACK_OK, "TM");
    radioSendTelemetry(d);

  } else if (cmd == "SYS") {
    String s = "reset=" + String(resetReasonName());
    s += " heap=" + String(ESP.getFreeHeap());
    s += " fs=" + String(LittleFS.usedBytes()) + "/" + String(LittleFS.totalBytes());
    s += " up=" + String(millis() / 1000) + "s";
    s += " frames=" + String(frameCounter);
    s += " rcu=" + rcu.describe();
    radioSendAck(seq, MSK_ACK_OK, s);

  } else if (cmd == "MODE") {
    String v = a1; v.toUpperCase();
    if      (v == "OFF")    radioCfg.mode = RADIO_OFF;
    else if (v == "BEACON") radioCfg.mode = RADIO_BEACON;
    else if (v == "STREAM") radioCfg.mode = RADIO_STREAM;
    else if (v == "FULL")   radioCfg.mode = RADIO_FULL;
    else { radioSendAck(seq, MSK_ACK_ERR, "mode must be OFF|BEACON|STREAM|FULL"); return; }
    radioSaveConfig();
    radioSendAck(seq, MSK_ACK_OK, "mode=" + v +
                 (radioCfg.mode == RADIO_OFF ? " (until the next reboot)" : ""));

  } else if (cmd == "TMRATE") {
    long v = a1.toInt();
    if (v < 250 || v > 600000) { radioSendAck(seq, MSK_ACK_ERR, "250..600000 ms"); return; }
    radioCfg.telemetry_ms = v;
    radioSaveConfig();
    radioSendAck(seq, MSK_ACK_OK, "tmrate=" + String(v));

  } else if (cmd == "BCRATE") {
    long v = a1.toInt();
    if (v < 1000 || v > 3600000) { radioSendAck(seq, MSK_ACK_ERR, "1000..3600000 ms"); return; }
    radioCfg.beacon_ms = v;
    radioSaveConfig();
    radioSendAck(seq, MSK_ACK_OK, "bcrate=" + String(v));

  // ---- platform -----------------------------------------------------------
  } else if (cmd == "SOLAR") {
    String v = a1; v.toUpperCase();
    bool want = stateMotor;
    if      (v == "DEPLOY")  want = true;
    else if (v == "RETRACT") want = false;
    else if (v == "TOGGLE")  want = !stateMotor;
    else { radioSendAck(seq, MSK_ACK_ERR, "DEPLOY|RETRACT|TOGGLE"); return; }
    if (setStateMotor(want)) radioSendAck(seq, MSK_ACK_OK, want ? "deploying" : "retracting");
    else                     radioSendAck(seq, MSK_ACK_BUSY, "servo cooling down or already there");

  } else if (cmd == "LED") {
    String v = a1; v.toUpperCase();
    if (v == "ON")        { stateLight = true;  control_light(true); }
    else if (v == "OFF")  { stateLight = false; control_light(false); }
    else if (v == "BLINK"){ startBlink(stateLight); }
    else { radioSendAck(seq, MSK_ACK_ERR, "ON|OFF|BLINK"); return; }
    radioSendAck(seq, MSK_ACK_OK, "led=" + v);

  } else if (cmd == "CAL") {
    radioSendAck(seq, MSK_ACK_OK, "calibrating - hold still 5 s");
    calibrateMPU();
    radioSendEvent("INFO", "MPU calibration complete");

  } else if (cmd == "TIME") {
    // TIME,<yyyy>,<mm>,<dd>,<hh>,<mi>,<ss>
    int y = a1.toInt(), mo = a2.toInt(), dd = a3.toInt();
    int hh = m.at(6).toInt(), mi = m.at(7).toInt(), ss = m.at(8).toInt();
    if (y < 2000 || y > 2100 || mo < 1 || mo > 12 || dd < 1 || dd > 31 ||
        hh > 23 || mi > 59 || ss > 59) {
      radioSendAck(seq, MSK_ACK_ERR, "bad date/time"); return;
    }
    setRTC(y, mo, dd, hh, mi, ss);
    radioSendAck(seq, MSK_ACK_OK, "rtc set");

  } else if (cmd == "CALLSIGN") {
    if (a1.length() == 0 || a1.length() > 12) { radioSendAck(seq, MSK_ACK_ERR, "1..12 chars"); return; }
    callSign = a1;
    saveCallSign(callSign);
    if (rcu.present() && !rcu.legacy()) rcu.setCallsign(callSign);
    radioSendAck(seq, MSK_ACK_OK, "callsign=" + callSign);

  // ---- payload ------------------------------------------------------------
  } else if (cmd == "SNAP") {
    String err;
    if (radioCapture(err)) {
      radioSendAck(seq, MSK_ACK_OK, "photo #" + String(globalPhotoCounter));
      if (a1.equalsIgnoreCase("DOWN")) radioImgStart(globalPhotoCounter);
    } else {
      radioSendAck(seq, MSK_ACK_ERR, err);
    }

  } else if (cmd == "IMGLIST") {
    String ids = "";
    for (size_t i = 0; i < photoRecords.size(); i++) {
      if (i) ids += ' ';
      ids += String(photoRecords[i].id);
    }
    radioSendAck(seq, MSK_ACK_OK, ids.length() ? ids : "(none)");

  } else if (cmd == "IMG") {
    int id = a1.toInt();
    if (imgTx.active)          radioSendAck(seq, MSK_ACK_BUSY, "downlink in progress");
    else if (radioImgStart(id))radioSendAck(seq, MSK_ACK_OK, "downlink #" + String(id));
    else                       radioSendAck(seq, MSK_ACK_ERR, "no photo #" + String(id));

  } else if (cmd == "IMGABORT") {
    if (imgTx.active) { radioImgAbort("by command"); radioSendAck(seq, MSK_ACK_OK, "aborted"); }
    else radioSendAck(seq, MSK_ACK_OK, "idle");

  } else if (cmd == "IMGPACE") {
    long v = a1.toInt();
    if (v < 0 || v > 2000) { radioSendAck(seq, MSK_ACK_ERR, "0..2000 ms"); return; }
    radioCfg.img_pace_ms = v;
    radioSaveConfig();
    radioSendAck(seq, MSK_ACK_OK, "pace=" + String(v));

  // ---- mission data logger ------------------------------------------------
  } else if (cmd == "LOGSTART") {
    long p = a1.toInt();
    if (p < 1) p = 5;
    logger.period_seconds = p;
    if (createNewLogFile()) {
      logger.enabled = true;
      logger.last_log_time = millis();
      saveLoggerState();
      radioSendAck(seq, MSK_ACK_OK, "logging every " + String(p) + " s");
    } else {
      radioSendAck(seq, MSK_ACK_ERR, "cannot create log file");
    }

  } else if (cmd == "LOGSTOP") {
    stopLogging();
    radioSendAck(seq, MSK_ACK_OK, "logging stopped");

  } else if (cmd == "LOGLIST") {
    String out = "";
    File root = LittleFS.open("/");
    File file = root.openNextFile();
    while (file) {
      String n = file.name();
      if (n.startsWith("mdata_") || n.startsWith("/mdata_")) {
        if (out.length()) out += ' ';
        out += n + ":" + String(file.size());
      }
      file = root.openNextFile();
    }
    radioSendAck(seq, MSK_ACK_OK, out.length() ? out : "(none)");

  } else if (cmd == "LOGDEL") {
    deleteMissionLogs();
    radioSendAck(seq, MSK_ACK_OK, "mission logs deleted");

  } else if (cmd == "EVLOG") {
    radioSendAck(seq, MSK_ACK_OK, "dumping event log");
    File f = LittleFS.open(EVENT_LOG_FILE, "r");
    if (f) {
      while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length()) radioSendEvent("LOG", line);
      }
      f.close();
    }

  // ---- radio controller (RCU) --------------------------------------------
  } else if (cmd == "RFSTAT") {
    rcu.refresh();
    radioSendAck(seq, MSK_ACK_OK, rcu.describe());
    radioSendRadioStatus();

  } else if (cmd == "RFCH" || cmd == "RFPWR" || cmd == "RFFU") {
    if (!rcu.present()) { radioSendAck(seq, MSK_ACK_ERR, "no radio controller for the SET pin"); return; }
    int v = a1.toInt();
    // The reply must go out BEFORE the link parameters change, otherwise the
    // ground station is already on the old channel when the ack arrives.
    radioSendAck(seq, MSK_ACK_OK, cmd + "=" + String(v) + " applying - follow me");
    delay(140);
    // Driven by the OBC, not the RCU: on board 1.5.8 the ATmega328P's UART is
    // not on the HC-12 bus, but the ESP32's is. See hc12_at.h.
    bool ok;
    if      (cmd == "RFCH")   ok = hc12SetChannel((uint8_t)v);
    else if (cmd == "RFPWR")  ok = hc12SetTxPower((uint8_t)v);
    else                      ok = hc12SetFuMode((uint8_t)v);
    radioSendEvent(ok ? "INFO" : "WARN", cmd + (ok ? " applied" : " REFUSED by the module"));

  } else if (cmd == "RFSYNC") {
    radioSendAck(seq, MSK_ACK_OK, "sweeping baud rates to find the HC-12");
    delay(140);
    String report;
    bool ok = hc12Sync(report);
    radioSendEvent(ok ? "INFO" : "WARN", "RFSYNC: " + report);

  } else if (cmd == "RFAT") {
    // Arbitrary AT passthrough - the diagnostic that would have found the
    // RCU wiring problem in one command instead of an afternoon.
    if (a1.length() == 0) { radioSendAck(seq, MSK_ACK_ERR, "usage: RFAT AT+RX"); return; }
    String at = a1;
    for (uint8_t i = 4; i < m.count; i++) {      // rejoin args split on commas
      if (m.at(i).length()) at += m.at(i);
    }
    // Query commands answer without the word "OK" - AT+V returns the version
    // banner, for example - so any reply at all counts as a success here.
    AtResult r = hc12SendAt(at, 1200, true);
    radioSendAck(seq, r.reply.length() ? MSK_ACK_OK : MSK_ACK_ERR,
                 r.reply.length() ? r.reply : "no reply from the module");

  } else if (cmd == "RFCFG") {
    radioSendAck(seq, MSK_ACK_OK, hc12ReadConfig());

  } else if (cmd == "RFTEST") {
    radioRfSelfTest(seq);

  } else if (cmd == "RFRESET") {
    if (!rcu.present()) { radioSendAck(seq, MSK_ACK_ERR, "no radio controller"); return; }
    radioSendAck(seq, MSK_ACK_OK, "power-cycling HC-12");
    delay(120);
    rcu.rfReset();

  } else if (cmd == "WDT") {
    if (!rcu.present() || rcu.legacy()) { radioSendAck(seq, MSK_ACK_ERR, "no MSK radio controller"); return; }
    long v = a1.toInt();
    if (v < 0 || v > 3600) { radioSendAck(seq, MSK_ACK_ERR, "0..3600 s"); return; }
    rcu.setWatchdog((uint16_t)v);
    radioSendAck(seq, MSK_ACK_OK, v ? ("watchdog " + String(v) + " s") : "watchdog off");

  } else if (cmd == "EMERG") {
    if (!rcu.present() || rcu.legacy()) { radioSendAck(seq, MSK_ACK_ERR, "no MSK radio controller"); return; }
    bool on = a1.toInt() != 0 || a1.equalsIgnoreCase("ON");
    rcu.armEmergency(on);
    radioSendAck(seq, MSK_ACK_OK, on ? "RCU emergency commanding armed" : "RCU emergency commanding disarmed");

  // ---- system -------------------------------------------------------------
  // ---- network ------------------------------------------------------------
  // The stock SetWIFI command is an interactive serial prompt, which is no use
  // over a radio link. This configures Wi-Fi in one frame.
  } else if (cmd == "WIFI") {
    if (a1.length() == 0) {
      radioSendAck(seq, MSK_ACK_ERR, "usage: WIFI <ssid> <password>");
      return;
    }
    // Ack first: associating blocks for several seconds.
    radioSendAck(seq, MSK_ACK_OK, "associating with " + a1);
    ssid = a1;
    password = a2;
    useWiFi = "Yes";
    saveWiFiConfig(ssid, password, useWiFi);

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) delay(250);

    if (WiFi.status() == WL_CONNECTED) {
      initServer();
      radioSendEvent("INFO", "WIFI up: http://" + WiFi.localIP().toString() +
                             "/ rssi " + String(WiFi.RSSI()) + " dBm");
      writeEventLog("WIFI_UP");
    } else {
      radioSendEvent("WARN", "WIFI could not associate with " + ssid);
    }

  } else if (cmd == "WIFISTAT") {
    if (WiFi.status() == WL_CONNECTED) {
      radioSendAck(seq, MSK_ACK_OK, "connected " + WiFi.SSID() + " http://" +
                   WiFi.localIP().toString() + "/ rssi " + String(WiFi.RSSI()) + " dBm");
    } else {
      radioSendAck(seq, MSK_ACK_OK, "not connected (useWiFi=" + useWiFi + ")");
    }

  } else if (cmd == "WIFIOFF") {
    saveWiFiConfig("none", "none", "No");
    useWiFi = "No";
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    radioSendAck(seq, MSK_ACK_OK, "wifi disabled - radio link unaffected");
    writeEventLog("WIFI_OFF");

  } else if (cmd == "FS") {
    String s = "total=" + String(LittleFS.totalBytes()) +
               " used=" + String(LittleFS.usedBytes()) +
               " photos=" + String(photoRecords.size()) + "/" + String(MAX_PHOTOS);
    radioSendAck(seq, MSK_ACK_OK, s);

  } else if (cmd == "REBOOT") {
    radioSendAck(seq, MSK_ACK_OK, "rebooting");
    writeEventLog("REBOOT_BY_TELECOMMAND");
    delay(400);
    ESP.restart();

  } else if (cmd == "SAFE") {
    radioCfg.mode = RADIO_BEACON;
    radioCfg.beacon_ms = 30000;
    if (imgTx.active) radioImgAbort("safe mode");
    stopLogging();
    control_light(false);
    stateLight = false;
    radioSaveConfig();
    writeEventLog("SAFE_MODE");
    radioSendAck(seq, MSK_ACK_OK, "safe mode: beacon only @30 s");

  } else if (cmd == "HELP") {
    radioSendAck(seq, MSK_ACK_OK,
      "PING TM SYS MODE TMRATE BCRATE SOLAR LED CAL TIME CALLSIGN SNAP "
      "IMGLIST IMG IMGABORT IMGPACE LOGSTART LOGSTOP LOGLIST LOGDEL EVLOG "
      "RFSTAT RFCH RFPWR RFFU RFSYNC RFAT RFCFG RFTEST RFRESET WDT EMERG "
      "WIFI WIFISTAT WIFIOFF FS SAFE REBOOT");

  } else {
    radioSendAck(seq, MSK_ACK_UNKNOWN, "unknown command " + cmd);
    return;
  }

  writeEventLog("TC \"" + cmd + "\"");
}

// ---------------------------------------------------------------------------
// Serial pump. This is the ONLY reader of Serial in the firmware: it splits the
// incoming stream into MSK frames (handled here) and plain lines (handed to the
// legacy human console so every stock command keeps working).
// ---------------------------------------------------------------------------
void radioSerialTask() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();

    if (mskRx.feed(c, mskMsg)) {
      if (mskMsg.type == MSK_T_TELECMD) radioDispatchTc(mskMsg);
      consoleLine = "";
      continue;
    }

    if (c == '\r') continue;
    if (c == '\n') {
      consoleLine.trim();
      if (consoleLine.length() && consoleLine[0] != '$') handleConsoleLine(consoleLine);
      consoleLine = "";
    } else if (consoleLine.length() < 160) {
      consoleLine += c;
    }
  }
}

// ---------------------------------------------------------------------------
// Console commands added on top of the stock set. Returns true if handled.
// ---------------------------------------------------------------------------
bool mskConsoleCommand(const String& in) {
  String cmd = in;
  int sp = cmd.indexOf(' ');
  String arg = (sp > 0) ? cmd.substring(sp + 1) : "";
  if (sp > 0) cmd = cmd.substring(0, sp);
  arg.trim();

  if (cmd.equalsIgnoreCase("RadioMode")) {
    String v = arg; v.toUpperCase();
    if      (v == "OFF")    radioCfg.mode = RADIO_OFF;
    else if (v == "BEACON") radioCfg.mode = RADIO_BEACON;
    else if (v == "STREAM") radioCfg.mode = RADIO_STREAM;
    else if (v == "FULL")   radioCfg.mode = RADIO_FULL;
    else { Serial.println("Usage: RadioMode OFF|BEACON|STREAM|FULL"); return true; }
    radioSaveConfig();
    reactToCommand("Radio mode: " + v);
    return true;
  }

  if (cmd.equalsIgnoreCase("RadioStatus")) {
    rcu.refresh();
    Serial.println("\n===== RADIO =====");
    Serial.println("  link      : " MSK_VERSION);
    Serial.print  ("  mode      : ");
    Serial.println(radioCfg.mode == RADIO_OFF ? "OFF" :
                   radioCfg.mode == RADIO_BEACON ? "BEACON" :
                   radioCfg.mode == RADIO_STREAM ? "STREAM" : "FULL");
    Serial.println("  beacon    : " + String(radioCfg.beacon_ms) + " ms");
    Serial.println("  telemetry : " + String(radioCfg.telemetry_ms) + " ms");
    Serial.println("  controller: " + rcu.describe());
    Serial.println("  frames tx : " + String(txSeq));
    Serial.println("  frames rx : " + String(mskRx.framesOk()) + " ok / " +
                   String(mskRx.framesBad()) + " bad");
    Serial.println("  telecmds  : " + String(rxTcCount) + " (" + String(rxTcRejected) + " not for us)");
    Serial.println("=================\n");
    return true;
  }

  if (cmd.equalsIgnoreCase("RadioChannel") || cmd.equalsIgnoreCase("RadioPower") ||
      cmd.equalsIgnoreCase("RadioFU")) {
    if (!rcu.present()) {
      Serial.println("No radio controller on I2C 0x08 - cannot reach the HC-12 SET pin.");
      return true;
    }
    int v = arg.toInt();
    bool ok;
    if      (cmd.equalsIgnoreCase("RadioChannel")) ok = hc12SetChannel((uint8_t)v);
    else if (cmd.equalsIgnoreCase("RadioPower"))   ok = hc12SetTxPower((uint8_t)v);
    else                                           ok = hc12SetFuMode((uint8_t)v);
    Serial.println(ok ? "OK - module accepted the setting"
                      : "FAILED - the HC-12 did not answer (check power and the SET line)");
    return true;
  }

  if (cmd.equalsIgnoreCase("RadioSync")) {
    String report;
    bool ok = hc12Sync(report);
    Serial.println((ok ? "OK: " : "FAILED: ") + report);
    return true;
  }

  if (cmd.equalsIgnoreCase("RadioConfig")) {
    Serial.println("HC-12 reports:");
    Serial.println(hc12ReadConfig());
    return true;
  }

  if (cmd.equalsIgnoreCase("RadioAT")) {
    if (arg.length() == 0) { Serial.println("Usage: RadioAT AT+RX"); return true; }
    AtResult r = hc12SendAt(arg, 900);
    Serial.println(r.ok ? ("OK: " + r.reply) : ("no/!OK reply: " + r.reply));
    return true;
  }

  if (cmd.equalsIgnoreCase("RadioReset")) {
    rcu.rfReset();
    reactToCommand("HC-12 power-cycled.");
    return true;
  }

  if (cmd.equalsIgnoreCase("Snap")) {
    String err;
    Serial.println(radioCapture(err) ? "Photo stored." : ("Capture failed: " + err));
    return true;
  }

  if (cmd.equalsIgnoreCase("Downlink")) {
    int id = arg.toInt();
    Serial.println(radioImgStart(id) ? "Downlink started." : "No such photo.");
    return true;
  }

  return false;
}

// ---------------------------------------------------------------------------
// Periodic work
// ---------------------------------------------------------------------------
void radioInit() {
  radioLoadConfig();
  rcu.begin();
  if (rcu.present() && !rcu.legacy()) {
    rcu.setCallsign(callSign);
    rcu.setWatchdogAction(RCU_WDT_BOTH);
  }
  lastBeacon = millis();
  lastTmTx   = millis();
  rfSelfTestAt = millis() + 20000;   // let the link settle first
  radioSendEvent("INFO", String("BOOT ") + FIRMWARE_VERSION + " " + MSK_VERSION +
                         " reset=" + resetReasonName());
}

void radioTask(pointer_of_sensors* data) {
  unsigned long now = millis();

  if (radioCfg.mode != RADIO_OFF && now - lastBeacon >= radioCfg.beacon_ms) {
    lastBeacon = now;
    radioSendBeacon(data);
    if (rcu.present() && !rcu.legacy()) rcu.kickWatchdog();
  }

  if ((radioCfg.mode == RADIO_STREAM || radioCfg.mode == RADIO_FULL) &&
      now - lastTmTx >= radioCfg.telemetry_ms && !imgTx.active) {
    lastTmTx = now;
    radioSendTelemetry(data);
  }

  if (!rfSelfTestDone && rfSelfTestAt && now >= rfSelfTestAt && !imgTx.active) {
    rfSelfTestDone = true;
    radioRfSelfTest();
  }

  radioImgTask();
  rcu.poll();
}

#endif  // MSK_RADIO_H
