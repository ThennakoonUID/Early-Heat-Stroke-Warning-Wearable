#include <Arduino.h>
#include <Wire.h>

void setup() {
  Serial.begin(115200);
  while (!Serial); // Wait for serial monitor to open
  
  Serial.println("\nI2C Scanner started.");
  
  // Initialize I2C bus. 
  // For ESP32, this defaults to SDA = 21, SCL = 22.
  Wire.begin(); 
}

void loop() {
  byte error, address;
  int nDevices = 0;

  Serial.println("Scanning...");

  // I2C addresses range from 1 to 127
  for (address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    error = Wire.endTransmission();

    if (error == 0) {
      Serial.print("I2C device found at address 0x");
      if (address < 16) {
        Serial.print("0");
      }
      Serial.println(address, HEX);
      nDevices++;
    } else if (error == 4) {
      Serial.print("Unknown error at address 0x");
      if (address < 16) {
        Serial.print("0");
      }
      Serial.println(address, HEX);
    }
  }

  if (nDevices == 0) {
    Serial.println("No I2C devices found.\n");
  } else {
    Serial.println("Scan complete.\n");
  }
  
  delay(5000); // Wait 5 seconds before scanning again
}