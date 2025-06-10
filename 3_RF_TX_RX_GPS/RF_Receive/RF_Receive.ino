
#include "RH_ASK.h"  // Include RadioHead Amplitude Shift Keying Library
#include "SPI.h"     // Include dependant SPI Library
#include "LiquidCrystal_I2C.h"

// Pin definitions
const int rxPin = 11;    // RF receiver pin (DATA)
const int ledPin = 13;   // LED pin

// RadioHead ASK object (txPin (not used), rxPin, pttPin (not used))
RH_ASK rfDriver(2000, rxPin, 0); // 2000 bps, RX on pin 11, no TX pin

// LCD setup (16x2, I2C address 0x27)
LiquidCrystal_I2C lcd(0x27, 16, 2);

// Variables to store last received location
float lastLatitude = 0.0;
float lastLongitude = 0.0;
bool locationReceived = false;

// Timing for alternating GPS and Google Maps display
unsigned long lastDisplayToggle = 0;
const unsigned long displayToggleInterval = 2000; // 2 seconds
bool showGPS = true;

void setup() {
  pinMode(ledPin, OUTPUT);
  
  // Initialize Serial for debugging
  Serial.begin(9600);
  Serial.println("Receiver starting...");
  
  // Initialize RF receiver
  if (!rfDriver.init()) {
    Serial.println("RF433MHz receiver initialization failed!");
    while (1); // Halt if initialization fails
  }
  Serial.println("RF433MHz receiver initialized.");
  delay(10);
  // Initialize LCD
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

void displayGPSOnLCD(float lat, float lon) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Lat=");
  lcd.print(lat, 4);
  lcd.setCursor(0, 1);
  lcd.print("Lon=");
  lcd.print(lon, 4);
}

void displayGoogleMapsOnLCD(float lat, float lon) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("G=");
  lcd.print(lat, 4);
  lcd.print(",");
  lcd.print(lon, 4);
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
  
  // Check for received message
  if (rfDriver.recv(buf, &buflen)) {
    Serial.println("Message received!");
    
    // Blink LED twice to confirm receipt
    for (int i = 0; i < 2; i++) {
      digitalWrite(ledPin, HIGH);
      delay(200);
      digitalWrite(ledPin, LOW);
      delay(200);
    }
    
    // Null-terminate the received buffer
    buf[buflen] = '\0';
    char *msg = (char *)buf;
    
    // Check if the message is a GPS coordinate (contains a comma)
    float latitude, longitude;
    if (strchr(msg, ',') != NULL && sscanf(msg, "%f,%f", &latitude, &longitude) == 2) {
      // GPS coordinates received
      lastLatitude = latitude;
      lastLongitude = longitude;
      locationReceived = true;
      showGPS = true; // Start with GPS display
      lastDisplayToggle = millis();
      
      // Update LCD with GPS coordinates
      displayGPSOnLCD(latitude, longitude);
      
      // Print to Serial Monitor
      Serial.print("Received GPS: Lat=");
      Serial.print(latitude, 6);
      Serial.print(", Lon=");
      Serial.print(longitude, 6);
      Serial.print(", Google Maps: https://maps.google.com/?q=");
      Serial.print(latitude, 6);
      Serial.print(",");
      Serial.println(longitude, 6);
    } else {
      // Non-GPS message (e.g., Namaskaram)
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Recvd:"+String(msg));
      if (strlen(msg) > 10) {
        lcd.setCursor(0, 1);
        lcd.print(msg + 10);
      }
      
      // Print to Serial Monitor
      Serial.print("Received message: ");
      Serial.println(msg);
      
      // Restore last GPS coordinates or "No location received" after 2 seconds
      delay(2000);
      lcd.clear();
      if (locationReceived) {
        displayGPSOnLCD(lastLatitude, lastLongitude);
        showGPS = true;
        lastDisplayToggle = millis();
      } else {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("No location");
        lcd.setCursor(0, 1);
        lcd.print("received");
      }
    }
  }
  
  // Update LCD display for alternating GPS and Google Maps link
  updateLCDDisplay();
}
