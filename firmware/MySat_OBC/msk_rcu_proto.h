/*
 * msk_rcu_proto.h - I2C protocol between the OBC (ESP32-CAM, master) and the
 *                   RCU / Radio Control Unit (ATmega328P, slave 0x08).
 *
 * !! This file is byte-identical in firmware/MySat_OBC and firmware/MySat_RCU.
 *    If you change one, copy it to the other.
 *
 * Wire format
 * -----------
 * Legacy (stock MySat firmware, still accepted):
 *     [cmd]                       single byte, cmd < 0x08
 *
 * Extended:
 *     [0xA5][len][cmd][arg0..argN][xor]
 *       len  = number of bytes after `len` INCLUDING cmd and the xor byte
 *       xor  = XOR of every byte from `len` through the last arg
 *
 * Status is read back with Wire.requestFrom(RCU_I2C_ADDR, RCU_STATUS_LEN);
 * the reply is a fixed-size little-endian struct (see RcuStatus below).
 */
#ifndef MSK_RCU_PROTO_H
#define MSK_RCU_PROTO_H

#include <stdint.h>

#define RCU_I2C_ADDR        0x08
#define RCU_PROTO_VERSION   2
#define RCU_FRAME_SOF       0xA5
#define RCU_MAX_PAYLOAD     8

// --- legacy single-byte commands (bit-compatible with stock firmware) -------
#define RCU_CMD_MOTOR_OPEN      0x00
#define RCU_CMD_MOTOR_CLOSE     0x01
#define RCU_CMD_RF_TURN         0x02   // toggle HC-12 power
#define RCU_CMD_RF_SET          0x03   // enter HC-12 AT mode (stock behaviour)

// --- extended commands ------------------------------------------------------
#define RCU_CMD_RF_POWER        0x10   // arg0: 0=off 1=on
#define RCU_CMD_RF_AT_MODE      0x11   // arg0: 0=transparent 1=AT
#define RCU_CMD_RF_CHANNEL      0x12   // arg0: 1..100
#define RCU_CMD_RF_TXPOWER      0x13   // arg0: 1..8   (-1 dBm .. +20 dBm)
#define RCU_CMD_RF_BAUD         0x14   // arg0: index into RCU_BAUD_TABLE
#define RCU_CMD_RF_FU_MODE      0x15   // arg0: 1..4   (HC-12 FU1..FU4)
#define RCU_CMD_RF_RESET        0x16   // power-cycle the HC-12
#define RCU_CMD_RF_SLEEP        0x17   // arg0: 0=wake 1=AT+SLEEP
#define RCU_CMD_RF_APPLY        0x18   // run the queued AT batch now
#define RCU_CMD_RF_SYNC         0x19   // sweep bauds, find the HC-12, force it to 115200

#define RCU_CMD_SERVO_ANGLE     0x20   // arg0: degrees 0..180
#define RCU_CMD_SERVO_STOP      0x21
#define RCU_CMD_SERVO_LIMITS    0x22   // arg0: opened angle, arg1: closed angle

#define RCU_CMD_WDT_SET         0x30   // arg0: timeout seconds (0 = disabled)
#define RCU_CMD_WDT_KICK        0x31
#define RCU_CMD_WDT_ACTION      0x32   // arg0: 0=none 1=beacon 2=rf_reset 3=both

#define RCU_CMD_BUS_GRANT       0x40   // OBC goes silent, RCU may drive TX
#define RCU_CMD_BUS_RELEASE     0x41   // RCU releases TX back to high-Z
#define RCU_CMD_SET_CALLSIGN    0x42   // arg0..5: up to 6 ASCII chars

#define RCU_CMD_EMERGENCY_ARM   0x50   // arg0: 0=off 1=on - accept ground TCs
#define RCU_CMD_PING            0x51
#define RCU_CMD_CLEAR_ERROR     0x52

// --- RF state ---------------------------------------------------------------
#define RCU_RF_OFF          0
#define RCU_RF_ON           1
#define RCU_RF_SLEEPING     2

// --- AT engine state --------------------------------------------------------
#define RCU_AT_IDLE         0
#define RCU_AT_ENTERING     1
#define RCU_AT_SENDING      2
#define RCU_AT_WAITING      3
#define RCU_AT_LEAVING      4
#define RCU_AT_ERROR        5
#define RCU_AT_SYNCING      6

// --- servo state ------------------------------------------------------------
#define RCU_SERVO_OFF       0
#define RCU_SERVO_NEW       1
#define RCU_SERVO_TURNING   2
#define RCU_SERVO_DONE      3

// --- error codes ------------------------------------------------------------
#define RCU_ERR_NONE        0
#define RCU_ERR_BAD_CRC     1
#define RCU_ERR_BAD_CMD     2
#define RCU_ERR_BAD_ARG     3
#define RCU_ERR_NO_BUS      4   // AT batch requested without a bus grant
#define RCU_ERR_AT_TIMEOUT  5
#define RCU_ERR_AT_NAK      6
#define RCU_ERR_RF_DEAD     7   // HC-12 stopped answering, auto power-cycled

// --- watchdog actions -------------------------------------------------------
#define RCU_WDT_NONE        0
#define RCU_WDT_BEACON      1
#define RCU_WDT_RF_RESET    2
#define RCU_WDT_BOTH        3

// --- status block -----------------------------------------------------------
// Packed so the byte layout is identical on AVR and Xtensa.
#pragma pack(push, 1)
struct RcuStatus {
  uint8_t  magic;          // 0x5A
  uint8_t  proto;          // RCU_PROTO_VERSION
  uint8_t  fw_major;
  uint8_t  fw_minor;
  uint8_t  rf_state;       // RCU_RF_*
  uint8_t  at_state;       // RCU_AT_*
  uint8_t  rf_channel;     // 1..100
  uint8_t  rf_txpower;     // 1..8
  uint8_t  rf_baud_idx;
  uint8_t  rf_fu_mode;     // 1..4
  uint8_t  servo_state;    // RCU_SERVO_*
  uint8_t  servo_angle;    // degrees
  uint8_t  bus_owner;      // 0=OBC 1=RCU
  uint8_t  emergency_armed;
  uint8_t  wdt_action;
  uint8_t  last_error;     // RCU_ERR_*
  uint16_t wdt_timeout_s;
  uint16_t wdt_remaining_s;
  uint32_t uptime_s;
  uint16_t rf_resets;      // how often the HC-12 had to be power-cycled
  uint16_t ground_tc_count;// telecommands the RCU decoded off the air
  uint8_t rx_frames_ok;    // ANY valid MSK frame seen on the RCU's RX pin
  uint8_t rx_frames_bad;   // frames that started but failed the CRC
  uint8_t rx_bytes;        // raw bytes seen - zero means the pin is idle
};
#pragma pack(pop)

// 31 bytes. The whole block must fit in the ATmega328P's 32-byte TWI buffer or
// Wire.write() truncates it in onRequest() and the OBC reads a short reply.
static_assert(sizeof(RcuStatus) <= 32,
              "RcuStatus must fit in the AVR TWI buffer (BUFFER_LENGTH = 32)");

#define RCU_STATUS_LEN   ((uint8_t)sizeof(RcuStatus))
#define RCU_STATUS_MAGIC 0x5A

// HC-12 baud table - index used by RCU_CMD_RF_BAUD.
static const uint32_t RCU_BAUD_TABLE[] = {
  1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200
};
#define RCU_BAUD_COUNT   8
#define RCU_BAUD_DEFAULT 7   /* 115200 - matches the OBC console rate */

#endif  // MSK_RCU_PROTO_H
