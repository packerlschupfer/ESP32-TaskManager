// test_main.cpp - Main entry point for unit tests
#include <Arduino.h>
#include <unity.h>
#include <Logger.h>
#include <ConsoleBackend.h>

// Forward declaration of test runner
extern void runAllTests();
extern Logger testLogger;

void setup() {
    // Initialize serial for test output
    Serial.begin(115200);
    while (!Serial && millis() < 5000) {
        delay(10);
    }
    
    // Initialize logger
    testLogger.init(2048);
    testLogger.enableLogging(true);
    testLogger.setLogLevel(ESP_LOG_INFO);
    
    // Wait for system to stabilize
    delay(2000);
    
    Serial.println("\n\n");
    Serial.println("================================");
    Serial.println("   TaskManager Unit Test Suite  ");
    Serial.println("================================");
    Serial.println();
    
    // Run the tests
    runAllTests();
}

void loop() {
    // Nothing to do here
    delay(1000);
}