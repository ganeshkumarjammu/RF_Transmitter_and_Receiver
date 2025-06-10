#include "RH_ASK.h"
#include <SPI.h> // Include dependant SPI Library
#include "TinyGPS++.h"
#include <SoftwareSerial.h>

// Pin definitions
const int buttonPin = 7;     // Push button pin
const int txPin = 12;        // RF transmitter pin (DATA)
const int ledPin = 13;       // Onboard LED for status
const int gpsRxPin = 3;      // GPS RX pin (SoftwareSerial)
const int gpsTxPin = 4;      // GPS TX pin (SoftwareSerial)

// GPS variables
float latitude = 0.0;
float longitude = 0.0;


// RadioHead ASK object (txPin, rxPin (not used), pttPin (not used))
RH_ASK rfDriver(2000, 0, txPin); // 2000 bps, no RX pin, TX on pin 12

// GPS setup
TinyGPSPlus gps;
SoftwareSerial gpsSerial(gpsRxPin, gpsTxPin);
#define GPS_BAUD 9600

// Timing for Namaskaram message
unsigned long lastNamaskaramTime = 0;
const unsigned long namaskaramInterval = 5000; // 5 seconds


void setup() {
  pinMode(buttonPin, INPUT_PULLUP); // Button with internal pull-up
  pinMode(ledPin, OUTPUT);
  
  // Initialize Serial for debugging
  Serial.begin(9600);
  Serial.println("Transmitter starting...");
  
  // Initialize GPS Serial
  gpsSerial.begin(GPS_BAUD);
  Serial.println("GPS module initialized.");
  
  // Initialize RF transmitter
  if (!rfDriver.init()) {
    Serial.println("RF433MHz transmitter initialization failed!");
    while (1); // Halt if initialization fails
  }
  Serial.println("RF433MHz transmitter initialized.");
}

void loop() {
  // Continuously read GPS data
  while (gpsSerial.available() > 0) {
    char c = gpsSerial.read();
    Serial.print(c); // Print raw NMEA data for debugging
    if (gps.encode(c)) {
        if (gps.location.isValid()) {
        // Store latitude and longitude in float variables
        latitude = gps.location.lat();
        longitude = gps.location.lng();
        
        // Print GPS data and Google Maps link
        Serial.print("GPS Fix: Lat=");
        Serial.print(latitude, 6);
        Serial.print(", Lon=");
        Serial.print(longitude, 6);
        Serial.print(", Google Maps: https://maps.google.com/?q=");
        Serial.print(latitude, 6);
        Serial.print(",");
        Serial.println(longitude, 6);
      } else {
        Serial.println("Waiting for GPS fix...");
      }
    }
  }
  
  // Check if button is pressed (LOW because of pull-up)
  if (digitalRead(buttonPin) == LOW) {
    Serial.println("Button pressed, sending GPS data...");
    digitalWrite(ledPin, HIGH); // Indicate transmission
    sendGPSData();
    digitalWrite(ledPin, LOW);
    delay(500); // Debounce delay
  } else {
    // Send Namaskaram message periodically when button is not pressed
    unsigned long currentTime = millis();
    if (currentTime - lastNamaskaramTime >= namaskaramInterval) {
      Serial.println("Button not pressed, sending Namaskaram...");
      sendNamaskaram();
      lastNamaskaramTime = currentTime;
    }
  }
}

void sendGPSData() {
  if (gps.location.isValid()) {
    // Prepare message with latitude and longitude
    char msg[50];
    snprintf(msg, sizeof(msg), "%.6f,%.6f", 
             gps.location.lat(), gps.location.lng());
    
    Serial.print("Sending GPS data: ");
    Serial.println(msg);
    
    // Send message multiple times for reliability
    for (int i = 0; i < 3; i++) {
      rfDriver.send((uint8_t *)msg, strlen(msg));
      rfDriver.waitPacketSent();
      Serial.println("GPS transmission attempt " + String(i + 1) + " complete.");
      delay(50);
    }
  } else {
    Serial.println("No valid GPS data to send.");
  }
}

void sendNamaskaram() {
  const char *msg = "Namaskaram";
  Serial.print("Sending communication check: ");
  Serial.println(msg);
  
  // Send message multiple times for reliability
  for (int i = 0; i < 3; i++) {
    rfDriver.send((uint8_t *)msg, strlen(msg));
    rfDriver.waitPacketSent();
    Serial.println("Namaskaram transmission attempt " + String(i + 1) + " complete.");
    delay(50);
  }
}
