// test_main.cpp - Main test runner for TaskManager library
#include <Arduino.h>
#include <unity.h>

// Forward declarations for test runners
void runTaskManagerTests();
void runTaskRecoveryTests();
void runResourceLeakDetectorTests();
void runStackSizeTests();

void setUp() {
    // Global setup for all tests
}

void tearDown() {
    // Global teardown for all tests
}

void setup() {
    // Initialize serial for test output
    Serial.begin(115200);
    while (!Serial) {
        delay(10);
    }
    
    delay(2000); // Give time for serial monitor to connect
    
    Serial.println("\n=== TaskManager Library Unit Tests ===\n");
    
    UNITY_BEGIN();
    
    // Run all test suites
    Serial.println("\n--- Running TaskManager Core Tests ---");
    runTaskManagerTests();
    
    Serial.println("\n--- Running Task Recovery Tests ---");
    runTaskRecoveryTests();
    
    Serial.println("\n--- Running Resource Leak Detector Tests ---");
    runResourceLeakDetectorTests();
    
    Serial.println("\n--- Running Stack Size Fix Tests ---");
    runStackSizeTests();
    
    UNITY_END();
    
    Serial.println("\n=== All Tests Completed ===\n");
}

void loop() {
    // Nothing to do in loop for tests
    delay(1000);
}