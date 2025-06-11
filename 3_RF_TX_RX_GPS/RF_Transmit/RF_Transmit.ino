#include "RH_ASK.h"
#include <SPI.h> // Include dependant SPI Library
#include "TinyGPS++.h"
#include <SoftwareSerial.h>

// Pin definitions
const int buttonPin = 2;     // Button on pin 2 (interrupt INT0)
const int txPin = 12;        // RF transmitter pin (DATA)
const int ledPin = 13;       // LED pin
const int gpsRxPin = 3;      // GPS RX pin (SoftwareSerial)
const int gpsTxPin = 4;      // GPS TX pin (SoftwareSerial)

// RadioHead ASK object (txPin, rxPin (not used), pttPin (not used))
RH_ASK rfDriver(2000, 0, txPin); // 2000 bps, no RX pin, TX on pin 12

// GPS setup
TinyGPSPlus gps;
SoftwareSerial gpsSerial(gpsRxPin, gpsTxPin);
#define GPS_BAUD 9600

// Timing for Namaskaram and GPS send
unsigned long lastNamaskaramTime = 0;
unsigned long lastGPSTime = 0;
const unsigned long namaskaramInterval = 5000; // 5 seconds
const unsigned long gpsInterval = 1000;        // 1 second

// GPS variables
float latitude = 0.0;
float longitude = 0.0;

// send state
volatile bool isTransmitting = false;
volatile unsigned long lastButtonPress = 0;
const unsigned long debounceDelay = 200; // 200 ms debounce

void setup() {
  pinMode(buttonPin, INPUT_PULLUP); // Button with internal pull-up
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW); // LED off initially
  
  // Attach interrupt to button pin (falling edge)
  attachInterrupt(digitalPinToInterrupt(buttonPin), togglesend, FALLING);
  
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

void togglesend() {
  // Debounce button press
  unsigned long currentTime = millis();
  if (currentTime - lastButtonPress > debounceDelay) {
    isTransmitting = !isTransmitting;
    Serial.print("Button pressed, continuous GPS send: ");
    Serial.println(isTransmitting ? "ON" : "OFF");
    lastButtonPress = currentTime;
  }
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
  
  // Update LED state based on send
  digitalWrite(ledPin, isTransmitting ? HIGH : LOW);
  
  // send logic
  unsigned long currentTime = millis();
  if (isTransmitting) {
    // Send GPS data every 1 second
    if (currentTime - lastGPSTime >= gpsInterval) {
      sendGPSData();
      lastGPSTime = currentTime;
    }
  } else {
    // Send Namaskaram message every 5 seconds
    if (currentTime - lastNamaskaramTime >= namaskaramInterval) {
      Serial.println("Button not pressed, sending Namaskaram...");
      sendNamaskaram();
      lastNamaskaramTime = currentTime;
    }
  }
}

void sendGPSData() {
  if (gps.location.isValid() && latitude != 0.0 && longitude != 0.0 && !isnan(latitude) && !isnan(longitude)) {
    // Use String to format message
    String gpsMessage = String(latitude, 6) + "," + String(longitude, 6);
    
    Serial.print("Sending GPS data: ");
    Serial.println(gpsMessage);
    
    // Convert String to C-string for RadioHead
    const char* msg = gpsMessage.c_str();
    
    // Send message multiple times for reliability
    for (int i = 0; i < 5; i++) {
      rfDriver.send((uint8_t *)msg, strlen(msg));
      rfDriver.waitPacketSent();
      Serial.println("GPS send attempt " + String(i + 1) + " complete.");
      delay(50);
    }
  } else {
    Serial.println("No valid GPS  data to send.");
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
    Serial.println("Mesg send attempt " + String(i + 1) + " complete.");
    delay(50);
  }
}
