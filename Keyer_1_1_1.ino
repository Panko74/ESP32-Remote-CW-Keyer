#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <EEPROM.h>

U8X8_SSD1306_128X64_NONAME_HW_I2C u8x8(U8X8_PIN_NONE);

// --- CONFIGURAZIONE PIN ---
const int pinDot        = 2;
const int pinDash       = 3;
const int pinWpmUp      = 4;
const int pinWpmDown    = 5;
const int pinSelectMode = 6;
const int pinBuzzer     = 9;
const int pinBypass     = 8;
const int pinOutRadio   = 12;

// --- INDIRIZZI EEPROM ---
const int ADDR_WPM    = 0;
const int ADDR_MODE   = 1;
const int ADDR_BYPASS = 2;
const int ADDR_VOL    = 4;

// --- VARIABILI GLOBALI ---
int currentWpm;
int mode;
bool isBypassActive;
int volumeLevel; 
int dotLength;

const int volTable[] = {0, 2, 5, 12, 28, 60, 120, 250, 450, 650, 833};

#define IDLE 0
#define DASH 1
#define DOT  2
#define DELAY 3
int currElt = IDLE;
int nextElt = IDLE;
int lastElt = IDLE;
unsigned long currEltEndTime = 0;

unsigned long lastActivityTime = 0;
const unsigned long screenTimeout = 15000;
bool isDisplayOn = true;
bool needsRedraw = false;

bool dotMemory = false;
bool dashMemory = false;
bool lastSentWasDot = false;

// --- TIMER 1: PWM HARDWARE 600Hz ---
void setupTimer1() {
  pinMode(pinBuzzer, OUTPUT);
  TCCR1A = _BV(COM1A1) | _BV(WGM11);
  TCCR1B = _BV(WGM13) | _BV(CS11);
  ICR1 = 1666;
  OCR1A = 0;
}

void buzzerOn() {
  if (volumeLevel <= 0) OCR1A = 0;
  else OCR1A = volTable[volumeLevel];
}

void buzzerOff() { OCR1A = 0; }

// --- INTERFACCIA ---
void updateSettings() {
  u8x8.setPowerSave(0);
  isDisplayOn = true;
  lastActivityTime = millis();
  dotLength = 1200 / currentWpm;

  u8x8.setFont(u8x8_font_7x14B_1x2_f);
  u8x8.setCursor(0, 0);
  if (mode == 0)      u8x8.print("MOD: STRAIGHT ");
  else if (mode == 1) u8x8.print("MOD: IAMBIC A ");
  else if (mode == 2) u8x8.print("MOD: IAMBIC B ");
  else if (mode == 3) u8x8.print("MOD: BUG-SIM  "); 
  
  u8x8.setCursor(0, 2);
  u8x8.print(isBypassActive ? "TX: [PRACTICE]" : "TX: [ONLINE]  ");

  u8x8.setCursor(0, 4);
  u8x8.print("SPEED: "); u8x8.print(currentWpm); u8x8.print(" WPM ");

  u8x8.setCursor(0, 6);
  u8x8.print("VOL:   ");
  if (volumeLevel == 0) u8x8.print("OFF   ");
  else { u8x8.print(volumeLevel); u8x8.print(" / 10 "); }
}

void saveSettings() {
  EEPROM.update(ADDR_WPM, (uint8_t)currentWpm);
  EEPROM.update(ADDR_MODE, (uint8_t)mode);
  EEPROM.update(ADDR_BYPASS, (uint8_t)isBypassActive);
  EEPROM.update(ADDR_VOL, (uint8_t)volumeLevel);
  u8x8.clearDisplay();
  u8x8.drawString(2, 3, " >> SAVED! << ");
  delay(1000);
  u8x8.clearDisplay();
  needsRedraw = true;
}

void loadSettings() {
  currentWpm = EEPROM.read(ADDR_WPM);
  mode = EEPROM.read(ADDR_MODE);
  isBypassActive = EEPROM.read(ADDR_BYPASS);
  volumeLevel = EEPROM.read(ADDR_VOL);

  if (currentWpm < 5 || currentWpm > 70) currentWpm = 20;
  if (mode > 3) mode = 1;
  if (volumeLevel < 0 || volumeLevel > 10) volumeLevel = 5;
}

void sendKey(bool state) {
  if (state) buzzerOn(); else buzzerOff();
  if (!isBypassActive) digitalWrite(pinOutRadio, state);
  else digitalWrite(pinOutRadio, LOW);
}

void playElementA(int multiplier) {
  sendKey(true);
  unsigned long start = millis();
  while(millis() - start < (unsigned long)dotLength * multiplier) {
    if (digitalRead(pinDot) == LOW) dotMemory = true;
    if (digitalRead(pinDash) == LOW) dashMemory = true;
  }
  sendKey(false);
  start = millis();
  while(millis() - start < (unsigned long)dotLength) {
    if (digitalRead(pinDot) == LOW) dotMemory = true;
    if (digitalRead(pinDash) == LOW) dashMemory = true;
  }
  lastSentWasDot = (multiplier == 1);
  if (digitalRead(pinDot) == HIGH && digitalRead(pinDash) == HIGH) {
    dotMemory = false; dashMemory = false;
  }
}

void setup() {
  pinMode(pinOutRadio, OUTPUT);
  pinMode(pinDot, INPUT_PULLUP);
  pinMode(pinDash, INPUT_PULLUP);
  pinMode(pinWpmUp, INPUT_PULLUP);
  pinMode(pinWpmDown, INPUT_PULLUP);
  pinMode(pinSelectMode, INPUT_PULLUP);
  pinMode(pinBypass, INPUT_PULLUP);
  setupTimer1();
  u8x8.begin();
  loadSettings();
  updateSettings();
}

void loop() {
  int dotVal = digitalRead(pinDot);
  int dashVal = digitalRead(pinDash);
  unsigned long now = millis();

  // Reset timer su QUALSIASI stimolo esterno (tasto CW o pulsanti)
  if (dotVal == LOW || dashVal == LOW) lastActivityTime = now;

  // --- MODALITA' STRAIGHT - PRIORITA' MASSIMA ---
  if (mode == 0) {
    sendKey(dotVal == LOW || dashVal == LOW);
    
    // Spegni display SOLO se tasti rilasciati e timeout scaduto
    if (dotVal == HIGH && dashVal == HIGH && isDisplayOn && (now - lastActivityTime > screenTimeout)) {
      u8x8.setPowerSave(1); isDisplayOn = false;
    }
    
    if (isDisplayOn && needsRedraw) { updateSettings(); needsRedraw = false; }

    bool bUp   = !digitalRead(pinWpmUp);
    bool bDown = !digitalRead(pinWpmDown);
    bool bMode = !digitalRead(pinSelectMode);
    bool bBy   = !digitalRead(pinBypass);

    if (bUp || bDown || bMode || bBy) {
      lastActivityTime = now;
      if (!isDisplayOn) { u8x8.setPowerSave(0); isDisplayOn = true; needsRedraw = true; delay(250); return; }

      if (bMode) {
        bool comboDetected = false;
        while (!digitalRead(pinSelectMode)) {
          if (!digitalRead(pinWpmUp)) {
            if (volumeLevel < 10) volumeLevel++; updateSettings();
            comboDetected = true; while(!digitalRead(pinWpmUp)); delay(50);
          }
          if (!digitalRead(pinWpmDown)) {
            if (volumeLevel > 0) volumeLevel--; updateSettings();
            comboDetected = true; while(!digitalRead(pinWpmDown)); delay(50);
          }
        }
        if (!comboDetected) { mode = (mode + 1) % 4; updateSettings(); }
        delay(50); return;
      }

      if (bBy) {
        unsigned long startP = millis();
        while(!digitalRead(pinBypass)) {
          if (millis() - startP > 1000) { saveSettings(); updateSettings(); return; }
        }
        isBypassActive = !isBypassActive; updateSettings(); delay(200); return;
      }

      if (bUp) { if (currentWpm < 70) currentWpm++; updateSettings(); delay(150); return; }
      if (bDown) { if (currentWpm > 5) currentWpm--; updateSettings(); delay(150); return; }
    }
    return;
  }

  // --- DISPLAY TIMEOUT (per modi Iambic/Bug) ---
  // Spegni SOLO se tasti rilasciati
  if (dotVal == HIGH && dashVal == HIGH && isDisplayOn && (now - lastActivityTime > screenTimeout)) {
    u8x8.setPowerSave(1); isDisplayOn = false;
  }

  // --- RESTO LOGICA MORSE ---
  if (mode == 1) { 
    if (dotVal == LOW && dashVal == LOW) {
      if (lastSentWasDot) playElementA(3); else playElementA(1);
    } else if (dotVal == LOW || dotMemory) {
      playElementA(1); dotMemory = false;
    } else if (dashVal == LOW || dashMemory) {
      playElementA(3); dashMemory = false;
    }
  } 
  else if (mode == 2) { 
    switch (currElt) {
      case IDLE:
        if (dotVal == LOW && dashVal == HIGH) { currElt = DOT; currEltEndTime = now + dotLength; }
        else if (dotVal == HIGH && dashVal == LOW) { currElt = DASH; currEltEndTime = now + 3 * dotLength; }
        else if (dotVal == LOW && dashVal == LOW) { currElt = DOT; nextElt = DASH; currEltEndTime = now + dotLength; }
        sendKey(false); break;
      case DOT:
        sendKey(true); if (dashVal == LOW) nextElt = DASH;
        if (now >= currEltEndTime) { lastElt = DOT; currElt = DELAY; currEltEndTime = now + dotLength; sendKey(false); lastSentWasDot = true; }
        break;
      case DASH:
        sendKey(true); if (dotVal == LOW) nextElt = DOT;
        if (now >= currEltEndTime) { lastElt = DASH; currElt = DELAY; currEltEndTime = now + dotLength; sendKey(false); lastSentWasDot = false; }
        break;
      case DELAY:
        sendKey(false);
        if (now >= currEltEndTime) {
          currElt = nextElt;
          if (currElt == DOT) currEltEndTime = now + dotLength;
          else if (currElt == DASH) currEltEndTime = now + 3 * dotLength;
          nextElt = IDLE;
        } else {
          if (lastElt == DOT && dashVal == LOW) nextElt = DASH;
          if (lastElt == DASH && dotVal == LOW) nextElt = DOT;
        }
        break;
    }
  }
  else if (mode == 3) { // MODO BUG-SIM
    if (dotVal == LOW || dotMemory) {
      playElementA(1); 
      dotMemory = false;
    }
    if (dashVal == LOW) {
      sendKey(true);
    } else if (dotVal == HIGH) { 
      sendKey(false);
    }
  }

  // --- MENU E TASTI ---
  if (currElt == IDLE && dotVal == HIGH && dashVal == HIGH) {
    if (isDisplayOn && needsRedraw) { updateSettings(); needsRedraw = false; }

    bool bUp   = !digitalRead(pinWpmUp);
    bool bDown = !digitalRead(pinWpmDown);
    bool bMode = !digitalRead(pinSelectMode);
    bool bBy   = !digitalRead(pinBypass);

    if (bUp || bDown || bMode || bBy) {
      lastActivityTime = now;
      if (!isDisplayOn) { u8x8.setPowerSave(0); isDisplayOn = true; needsRedraw = true; delay(250); return; }

      if (bMode) {
        bool comboDetected = false;
        while (!digitalRead(pinSelectMode)) {
          if (!digitalRead(pinWpmUp)) {
            if (volumeLevel < 10) volumeLevel++; updateSettings();
            comboDetected = true; while(!digitalRead(pinWpmUp)); delay(50);
          }
          if (!digitalRead(pinWpmDown)) {
            if (volumeLevel > 0) volumeLevel--; updateSettings();
            comboDetected = true; while(!digitalRead(pinWpmDown)); delay(50);
          }
        }
        if (!comboDetected) {
          mode = (mode + 1) % 4;
          updateSettings();
        }
        delay(50); return;
      }

      if (bBy) {
        unsigned long startP = millis();
        while(!digitalRead(pinBypass)) {
          if (millis() - startP > 1000) { saveSettings(); updateSettings(); return; }
        }
        isBypassActive = !isBypassActive; updateSettings(); delay(200); return;
      }

      if (bUp) { if (currentWpm < 70) currentWpm++; updateSettings(); delay(150); return; }
      if (bDown) { if (currentWpm > 5) currentWpm--; updateSettings(); delay(150); return; }
    }
  }
}
