//for communication with Nano (ATmega328p) microcontroller - MySat auxiliary microcontroller
//and for controlling the STAR LED - MySat debug indicator

//Nano (ATmega328p) is used for the operation of subsystems - control of the MySat solar panels movement and other tasks
#ifndef CONTROL_H
#define CONTROL_H

#include <EEPROM.h>
#include <Adafruit_NeoPixel.h>
#include <WiFi.h>

#define SIGNALLED_BRIGHTNESS 20
#define STARLED_BRIGHTNESS 65
// NOTE: ESP32 Arduino core 3.x made the LEDC API pin-based. ledcAttach(pin,..)
// and ledcWrite(pin,..) both take the GPIO now - there is no channel argument.
// Passing a channel number here wrote PWM to GPIO 0 instead, which on the
// AI-Thinker board is the camera's XCLK pin, so the STAR LED never lit while
// every status flag claimed it had. The old channel define is deliberately
// gone so it cannot be reintroduced.
#define EEPROM_ADDR_STATE_MOTOR 0

bool stateMotor;
unsigned long lastMotorChange = 0;
bool motorRestored = false;          // boot-time position restore, deferred
extern bool stateLight;              // defined in web_server.h

// Below this the servo is not worth attempting: its inrush is the largest
// transient on the board, and a sagging cell will brown the OBC out mid-move.
#define SERVO_MIN_BATTERY_V 3.65f

const int LED = 14;  //MySat STAR LED
const int SIGNAL_LED = 2;  //MySat SIGNAL LED
const int NUM_LEDS = 1;

Adafruit_NeoPixel signalStrip(NUM_LEDS, SIGNAL_LED, NEO_GRB + NEO_KHZ800);

enum LedMode { LED_OFF,
               LED_SOLID,
               LED_BLINK };

struct SignalLedState {
  uint8_t r, g, b;
  LedMode mode;
  uint8_t brightness;
  uint16_t onInterval;
  uint16_t offInterval;
  unsigned long lastUpdate;
  bool state;
};

SignalLedState signalLed;

void initSignalLed() {   //initializes the SIGNAL LED; used in setup()
  signalStrip.begin();
  signalStrip.setBrightness(SIGNALLED_BRIGHTNESS);
  signalStrip.show();
  signalLed = { 0, 0, 0, LED_OFF, SIGNALLED_BRIGHTNESS, 500, 500, millis(), false };
}

void setSignalLed(uint8_t r, uint8_t g, uint8_t b, LedMode mode,        //configures the SIGNAL LED settings
                  uint8_t brightness = SIGNALLED_BRIGHTNESS, uint16_t onInterval = 500, uint16_t offInterval = 0) {
  signalLed.r = r;
  signalLed.g = g;
  signalLed.b = b;
  signalLed.mode = mode;
  signalLed.brightness = brightness;
  signalLed.onInterval = onInterval;
  signalLed.offInterval = (offInterval == 0) ? onInterval : offInterval;
  signalLed.lastUpdate = millis();
  signalLed.state = true;  //used within LED_BLINK mode to create the blinking effect
  signalStrip.setBrightness(brightness);
}

void updateSignalLed() {      //tracks which LED mode should be used; 
  unsigned long now = millis();
  switch (signalLed.mode) {
    case LED_OFF:
      signalStrip.clear();
      break;
    case LED_SOLID:
      signalStrip.setPixelColor(0, signalStrip.Color(signalLed.r, signalLed.g, signalLed.b));
      break;
    case LED_BLINK:
      uint16_t currentInterval = signalLed.state ? signalLed.onInterval : signalLed.offInterval;
      if (now - signalLed.lastUpdate > currentInterval) {
        signalLed.state = !signalLed.state;
        signalLed.lastUpdate = now;
      }
      signalStrip.setPixelColor(0, signalLed.state ? signalStrip.Color(signalLed.r, signalLed.g, signalLed.b) : 0);
      break;
  }
  signalStrip.show();
}

void evaluateSystemState() {     //monitors the system state and triggers appropriate LED indication; 
  if (WiFi.status() != WL_CONNECTED) {
    setSignalLed(0, 0, 255, LED_BLINK, SIGNALLED_BRIGHTNESS, 800, 200);
    return;
  }

  setSignalLed(0, 0, 255, LED_SOLID);
}

unsigned long lastSystemCheck = 0;

void checkSystemState(){
  if(millis() - lastSystemCheck > 5000){
    evaluateSystemState();
    lastSystemCheck = millis();
  }
  updateSignalLed();
}

void control_light(bool state_light) {  //turns the STAR LED on or off based on the input parameter (true/false)
  if (state_light) {
    ledcWrite(LED, STARLED_BRIGHTNESS);
  } else {
    ledcWrite(LED, 0);
  }
}

void initStarLed() {  //initializes the STAR LED; used in setup()
  ledcAttach(LED, 5000, 8);
  control_light(false);
}

struct BlinkLedState{
  bool active;
  unsigned long lastBlink;
  uint8_t step;
  bool returnState;
};

BlinkLedState blinkLedState = {false, 0, 0, false};

void startBlink(bool current_state_light){
  blinkLedState.active = true;
  blinkLedState.lastBlink = millis();
  blinkLedState.step = 0;
  blinkLedState.returnState = current_state_light;
}

void updateBlinkStarLed(){
  if(!blinkLedState.active) return;

  unsigned long now = millis();

  if(now - blinkLedState.lastBlink < 300) return;

  switch(blinkLedState.step){
    case 0: control_light(false); break; 
    case 1: control_light(true); break;  
    case 2: control_light(false); break;  
    case 3: control_light(true); break;   
    case 4: control_light(false); break;  
    case 5:
      control_light(blinkLedState.returnState);  
      blinkLedState.active = false;
      return;
  }

  blinkLedState.step++;
  blinkLedState.lastBlink = now;
}

void control_motor(bool state_motor) {  //deploys or retracts the solar panels depending on the input parameter (true/false)
  if (state_motor) {
    Wire.beginTransmission(8);
    Wire.write(byte(0));
    Wire.endTransmission();
  } else {
    Wire.beginTransmission(8);
    Wire.write(byte(1));
    Wire.endTransmission();
  }
}

void loadStateMotor() {
  EEPROM.begin(4);  
  byte val = EEPROM.read(EEPROM_ADDR_STATE_MOTOR);
  stateMotor = (val == 1);
}

void saveStateMotor() {
  EEPROM.write(EEPROM_ADDR_STATE_MOTOR, stateMotor ? 1 : 0);
  EEPROM.commit(); 
}

// Returns the battery voltage, or -1 when there is no gauge to ask.
float batteryVoltage() {
  if (!init_status.ina_) return -1.0f;
  ina_struct* p = get_ina_data();
  return p->batteryVoltage;
}

bool servoPowerAvailable() {
  float v = batteryVoltage();
  return (v < 0.0f) || (v >= SERVO_MIN_BATTERY_V);
}

bool setStateMotor(bool newState) {
  unsigned long now = millis();
  if (now - lastMotorChange < 2200) return false;
  if (stateMotor == newState) return false;

  if (!servoPowerAvailable()) {
    LOG_WARN("[MOTOR] Refusing to move: battery " + String(batteryVoltage(), 2) +
             " V is below " + String(SERVO_MIN_BATTERY_V, 2) + " V");
    return false;
  }

  // Shed what we can before the inrush. The STAR LED is a high-current part
  // and there is no reason for it to be drawing while the servo starts.
  bool ledWas = stateLight;
  if (ledWas) control_light(false);

  stateMotor = newState;
  saveStateMotor();
  control_motor(stateMotor);
  lastMotorChange = now;

  if (ledWas) control_light(true);
  return true;
}

// The stock firmware commands the servo as the very first statement of
// setup(), before anything else is initialised. If that move browns the board
// out, the next boot repeats it - a reset loop that cannot be broken from the
// ground. Restoring the position a few seconds in, and only when the battery
// can take it, keeps the satellite reachable either way.
void restorePanelPosition() {
  if (motorRestored) return;
  motorRestored = true;
  if (!servoPowerAvailable()) {
    LOG_WARN("[MOTOR] Skipping boot-time panel restore: battery too low");
    return;
  }
  control_motor(stateMotor);
  lastMotorChange = millis();
}

void setRadio(){
  Wire.beginTransmission(8);
  Wire.write(byte(3));
  Wire.endTransmission();
}

#endif