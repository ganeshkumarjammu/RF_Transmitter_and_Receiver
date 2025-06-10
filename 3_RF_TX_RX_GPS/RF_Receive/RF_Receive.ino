
#include "RH_ASK.h"  // Include RadioHead Amplitude Shift Keying Library
#include "SPI.h"     // Include dependant SPI Library

// Pin definitions
const int rxPin = 11;    // RF receiver pin (DATA)
const int ledPin = 13;   // LED pin

// RadioHead ASK object (txPin (not used), rxPin, pttPin (not used))
RH_ASK rfDriver(2000, rxPin, 0); // 2000 bps, RX on pin 11, no TX pin

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
}

void loop() {
  uint8_t buf[RH_ASK_MAX_MESSAGE_LEN];
  uint8_t buflen = sizeof(buf);
  
  // Check for received message
  if (rfDriver.recv(buf, &buflen)) {
    Serial.println("Message received!");
        // Blink LED twice to confirm receipt
    digitalWrite(ledPin, HIGH);
    
    // Print received message
    Serial.print("Received message: ");
    for (int i = 0; i < buflen; i++) {
      Serial.write((char)buf[i]);
    }
    Serial.println();
    delay(2000);
    digitalWrite(ledPin, LOW);
    delay(200);
  }
}
