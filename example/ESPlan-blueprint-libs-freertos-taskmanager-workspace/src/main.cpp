// test_taskmanager_stack.cpp - Test TaskManager stack size fix
#include <Arduino.h>
#include <TaskManager.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

TaskManager taskManager;

// Test task that allocates 3KB on a 4KB stack
void StackTestTask(void* param) {
    const char* testName = (const char*)param;
    
    // Allocate 3000 bytes
    volatile uint8_t bigArray[3000];
    
    // Use the array to prevent optimization
    for (int i = 0; i < 3000; i++) {
        bigArray[i] = i & 0xFF;
    }
    
    // Check remaining stack
    UBaseType_t waterMark = uxTaskGetStackHighWaterMark(NULL);
    size_t freeBytes = waterMark * sizeof(StackType_t);
    
    Serial.printf("\n=== %s Results ===\n", testName);
    Serial.printf("Stack requested: 4096 bytes\n");
    Serial.printf("Array allocated: 3000 bytes\n");
    Serial.printf("Free stack found: %u bytes\n", freeBytes);
    Serial.printf("Expected free: ~1000 bytes\n");
    
    if (freeBytes > 800 && freeBytes < 1200) {
        Serial.printf("✓ PASS: Stack correctly allocated in BYTES!\n");
    } else if (freeBytes > 10000) {
        Serial.printf("✗ FAIL: Stack allocated in WORDS (4x too much)!\n");
        Serial.printf("  This indicates the bug is NOT fixed.\n");
    } else {
        Serial.printf("✗ FAIL: Unexpected stack size\n");
    }
    Serial.println("========================\n");
    
    vTaskDelay(pdMS_TO_TICKS(1000));
    vTaskDelete(NULL);
}

void setup() {
    Serial.begin(115200);
    while (!Serial) delay(10);
    delay(2000);
    
    Serial.println("\n\n=== Testing TaskManager Stack Size Fix ===");
    Serial.println("This verifies that TaskManager interprets stack size as BYTES");
    Serial.println("(Fixed the bug where it was passing bytes as words to FreeRTOS)\n");
    
    // Test 1: Create task with TaskManager
    Serial.println("Test 1: Using TaskManager.startTask()");
    bool result = taskManager.startTask(
        StackTestTask,
        "TMStack",
        4096,  // This should now be correctly interpreted as BYTES
        (void*)"TaskManager Test",
        1
    );
    
    if (!result) {
        Serial.println("ERROR: Failed to create task with TaskManager!");
    }
    
    delay(2000);
    
    // Test 2: Create pinned task with TaskManager
    Serial.println("\nTest 2: Using TaskManager.startTaskPinned()");
    result = taskManager.startTaskPinned(
        StackTestTask,
        "TMPinned",
        4096,  // This should also be correctly interpreted as BYTES
        (void*)"TaskManager Pinned Test",
        1,
        0  // Core 0
    );
    
    if (!result) {
        Serial.println("ERROR: Failed to create pinned task with TaskManager!");
    }
    
    delay(2000);
    
    // Show summary
    Serial.println("\n=== TEST SUMMARY ===");
    Serial.println("If both tests show ~1000 bytes free, the fix is working correctly.");
    Serial.println("If they show ~13000 bytes free, the bug is NOT fixed.");
    Serial.println("===================\n");
}

void loop() {
    static unsigned long lastPrint = 0;
    if (millis() - lastPrint > 5000) {
        lastPrint = millis();
        
        // Print task statistics
        auto stats = taskManager.getTaskStatistics();
        Serial.printf("Active tasks: %d\n", stats.totalTasks);
    }
    
    delay(100);
}