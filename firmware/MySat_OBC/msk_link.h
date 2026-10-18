/*
 * msk_link.h - MSK-1 link layer (framing + CRC) for the MySat radio link.
 *
 * The MySat boards put the ESP32 UART0, the ATmega328P UART and the HC-12
 * transceiver on ONE shared TX/RX pair (see MySatKit-PCB Schematic_1.5.8
 * board-A "Board B connector 2" and board-B "HC-12 connector" H5). Everything
 * the OBC prints therefore goes out over the air, and everything the ground
 * station transmits arrives at BOTH microcontrollers.
 *
 * MSK-1 keeps that property usable:
 *   - frames are printable ASCII, so the stock "open the Serial Monitor and
 *     read the telemetry" workflow still works;
 *   - frames are self-delimiting ('$' ... '\n') and CRC-protected, so a decoder
 *     can pick them out of a stream that also carries free-form console text;
 *   - a frame never contains '$', '*', ',' or '\n' inside a field.
 *
 * Frame:   $<TYPE>,<field>,<field>,...*<CRC16>\n
 *          CRC16-CCITT-FALSE over the bytes between '$' and '*', 4 hex digits.
 */
#ifndef MSK_LINK_H
#define MSK_LINK_H

#include <Arduino.h>

#define MSK_VERSION      "MSK-1"
#define MSK_MAX_FRAME    512
#define MSK_MAX_FIELDS   32   /* a TM frame carries 28 */

// ---- frame types -----------------------------------------------------------
#define MSK_T_BEACON     "BC"   // low-rate presence + health
#define MSK_T_TELEMETRY  "TM"   // full sensor frame
#define MSK_T_EVENT      "EV"   // asynchronous event / log line
#define MSK_T_ACK        "AK"   // response to an uplinked telecommand
#define MSK_T_IMG_META   "IM"   // start of an image downlink
#define MSK_T_IMG_DATA   "ID"   // one base64 chunk of an image
#define MSK_T_IMG_END    "IE"   // end of an image downlink + CRC32
#define MSK_T_TELECMD    "TC"   // uplink telecommand
#define MSK_T_RADIO      "RS"   // radio-controller status

// ---- ack status codes ------------------------------------------------------
#define MSK_ACK_OK       "OK"
#define MSK_ACK_ERR      "ERR"
#define MSK_ACK_BUSY     "BUSY"
#define MSK_ACK_UNKNOWN  "UNK"
#define MSK_ACK_DENIED   "DENY"

// ---------------------------------------------------------------------------
// CRC16-CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no final xor).
// ---------------------------------------------------------------------------
inline uint16_t mskCrc16(const char* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)((uint8_t)data[i]) << 8;
    for (uint8_t b = 0; b < 8; b++) {
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

inline uint16_t mskCrc16(const String& s) {
  return mskCrc16(s.c_str(), s.length());
}

// CRC32 (IEEE 802.3, reflected) - used to verify a reassembled image.
inline uint32_t mskCrc32Update(uint32_t crc, const uint8_t* buf, size_t len) {
  crc = ~crc;
  for (size_t i = 0; i < len; i++) {
    crc ^= buf[i];
    for (uint8_t b = 0; b < 8; b++) {
      crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320UL : (crc >> 1);
    }
  }
  return ~crc;
}

// ---------------------------------------------------------------------------
// Field sanitising. A field may not carry a frame delimiter.
// ---------------------------------------------------------------------------
inline String mskEscape(const String& in) {
  String out;
  out.reserve(in.length());
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == ',')       out += ';';
    else if (c == '$')  out += 'S';
    else if (c == '*')  out += '+';
    else if (c == '\r') continue;
    else if (c == '\n') out += ' ';
    else if ((uint8_t)c < 0x20 || (uint8_t)c > 0x7E) out += '?';
    else out += c;
  }
  return out;
}

// ---------------------------------------------------------------------------
// Builder. Usage:
//     MskFrame f(MSK_T_EVENT);
//     f.add(seq); f.add(callsign); f.add("BOOT");
//     Serial.print(f.finish());
// ---------------------------------------------------------------------------
class MskFrame {
 public:
  explicit MskFrame(const char* type) {
    body_.reserve(160);
    body_ = type;
  }

  MskFrame& add(const String& v) { body_ += ','; body_ += mskEscape(v); return *this; }
  MskFrame& add(const char* v)   { return add(String(v)); }
  MskFrame& add(long v)          { body_ += ','; body_ += String(v); return *this; }
  MskFrame& add(int v)           { return add((long)v); }
  MskFrame& add(unsigned long v) { body_ += ','; body_ += String(v); return *this; }
  MskFrame& add(bool v)          { body_ += ','; body_ += (v ? '1' : '0'); return *this; }
  MskFrame& add(float v, uint8_t decimals = 2) {
    body_ += ',';
    if (isnan(v) || isinf(v)) body_ += "nan";
    else body_ += String(v, (unsigned int)decimals);
    return *this;
  }
  // Adds an already-sanitised payload (base64 etc.) without re-scanning it.
  MskFrame& addRaw(const String& v) { body_ += ','; body_ += v; return *this; }

  // Returns "$<body>*<crc>\n".
  String finish() const {
    char crcbuf[8];
    snprintf(crcbuf, sizeof(crcbuf), "*%04X\n", mskCrc16(body_));
    String out;
    out.reserve(body_.length() + 8);
    out += '$';
    out += body_;
    out += crcbuf;
    return out;
  }

  size_t size() const { return body_.length() + 7; }

 private:
  String body_;
};

// ---------------------------------------------------------------------------
// Incremental parser. Feed it one byte at a time; it ignores anything that is
// not a well-formed frame, so free-form console text on the same wire is safe.
// ---------------------------------------------------------------------------
struct MskParsed {
  String type;
  String field[MSK_MAX_FIELDS];
  uint8_t count = 0;

  const String& at(uint8_t i) const {
    static String empty = "";
    return (i < count) ? field[i] : empty;
  }
  long   asLong(uint8_t i)  const { return at(i).toInt(); }
  float  asFloat(uint8_t i) const { return at(i).toFloat(); }
};

class MskParser {
 public:
  // Returns true once a valid frame has been decoded into `out`.
  bool feed(char c, MskParsed& out) {
    if (c == '$') {                 // (re)start - a '$' always begins a frame
      buf_ = "";
      active_ = true;
      return false;
    }
    if (!active_) return false;

    if (c == '\n' || c == '\r') {
      bool ok = active_ && finish(out);
      active_ = false;
      buf_ = "";
      return ok;
    }
    if (buf_.length() >= MSK_MAX_FRAME) {   // runaway - drop it
      active_ = false;
      buf_ = "";
      return false;
    }
    buf_ += c;
    return false;
  }

  unsigned long framesOk()  const { return okCount_; }
  unsigned long framesBad() const { return badCount_; }

 private:
  bool finish(MskParsed& out) {
    int star = buf_.lastIndexOf('*');
    if (star < 1 || buf_.length() - star != 5) { badCount_++; return false; }

    String body = buf_.substring(0, star);
    String crcHex = buf_.substring(star + 1);
    uint16_t want = (uint16_t)strtoul(crcHex.c_str(), nullptr, 16);
    if (mskCrc16(body) != want) { badCount_++; return false; }

    out.count = 0;
    int start = 0;
    int comma = body.indexOf(',');
    out.type = (comma < 0) ? body : body.substring(0, comma);
    if (comma < 0) { okCount_++; return true; }

    start = comma + 1;
    while (start <= (int)body.length() && out.count < MSK_MAX_FIELDS) {
      comma = body.indexOf(',', start);
      if (comma < 0) {
        out.field[out.count++] = body.substring(start);
        break;
      }
      out.field[out.count++] = body.substring(start, comma);
      start = comma + 1;
    }
    okCount_++;
    return true;
  }

  String buf_;
  bool active_ = false;
  unsigned long okCount_ = 0;
  unsigned long badCount_ = 0;
};

#endif  // MSK_LINK_H
