#include "RH_ASK.h"  // Include RadioHead Amplitude Shift Keying Library
#include "SPI.h"
#include "LiquidCrystal_I2C.h"

// Pin definitions
const int rxPin = 11;
const int ledPin = 13;

// RadioHead ASK object
RH_ASK rfDriver(2000, rxPin, 0); // 2000 bps, RX on pin 11

// LCD setup (16x2, I2C address 0x27)
LiquidCrystal_I2C lcd(0x27, 16, 2);

// Last known coordinates
float lastLatitude = 0.0;
float lastLongitude = 0.0;
bool locationReceived = false;

// Alternating display timing
unsigned long lastDisplayToggle = 0;
const unsigned long displayToggleInterval = 2000;
bool showGPS = true;

void setup() {
  pinMode(ledPin, OUTPUT);
  Serial.begin(9600);
  Serial.println("Receiver starting...");

  if (!rfDriver.init()) {
    Serial.println("RF433MHz receiver initialization failed!");
    while (1);
  }
  Serial.println("RF433MHz receiver initialized.");

  lcd.begin();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("  Geo-Location  ");
  lcd.setCursor(0, 1);
  lcd.print("    Tracker");
  delay(2000);
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("No location");
  lcd.setCursor(0, 1);
  lcd.print("received");
}

void displayGPSOnLCD(float latit, float lon) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Lat=");
  lcd.print(latit, 6);
  lcd.setCursor(0, 1);
  lcd.print("Lon=");
  lcd.print(lon, 6);
}

void displayGoogleMapsOnLCD(float latit, float lon) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Lat:");
  lcd.print(latit, 6);
  lcd.print("Lon:");
  lcd.setCursor(0, 1);
  lcd.print(lon, 6);
}

void updateLCDDisplay() {
  if (locationReceived) {
    unsigned long currentTime = millis();
    if (currentTime - lastDisplayToggle >= displayToggleInterval) {
      showGPS = !showGPS;
      lastDisplayToggle = currentTime;
      if (showGPS) {
        displayGPSOnLCD(lastLatitude, lastLongitude);
      } else {
        displayGoogleMapsOnLCD(lastLatitude, lastLongitude);
      }
    }
  }
}

void loop() {
  uint8_t buf[RH_ASK_MAX_MESSAGE_LEN];
  uint8_t buflen = sizeof(buf);

  if (rfDriver.recv(buf, &buflen)) {
    Serial.println("Message received!");

    for (int i = 0; i < 2; i++) {
      digitalWrite(ledPin, HIGH);
      delay(200);
      digitalWrite(ledPin, LOW);
      delay(200);
    }

    buf[buflen] = '\0';
    String msg = String((char *)buf);
    msg.trim();

    if (msg.indexOf(',') != -1) {
      int commaIndex = msg.indexOf(',');
      String latStr = msg.substring(0, commaIndex);
      String lonStr = msg.substring(commaIndex + 1);

      float latit = latStr.toFloat();
      float lon = lonStr.toFloat();

      if (latit != 0.0 && lon != 0.0) {
        lastLatitude = latit;
        lastLongitude = lon;
        locationReceived = true;
        showGPS = true;
        lastDisplayToggle = millis();

        displayGPSOnLCD(latit, lon);

        Serial.print("Received GPS: Lat=");
        Serial.print(latit, 6);
        Serial.print(", Lon=");
        Serial.print(lon, 6);
        Serial.println(", Google Maps: ");
        Serial.print("https://maps.google.com/?q=");
        Serial.print(latit, 6);
        Serial.print(",");
        Serial.println(lon, 6);
      }
    } else {
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Recvd:" + msg);
      if (msg.length() > 10) {
        lcd.setCursor(0, 1);
        lcd.print(msg.substring(10));
      }

      Serial.print("Received message: ");
      Serial.println(msg);

      delay(2000);
      lcd.clear();
      if (locationReceived) {
        displayGPSOnLCD(lastLatitude, lastLongitude);
        showGPS = true;
        lastDisplayToggle = millis();
      } else {
        lcd.setCursor(0, 0);
        lcd.print("No location");
        lcd.setCursor(0, 1);
        lcd.print("received");
      }
    }
  }

  updateLCDDisplay();
}
