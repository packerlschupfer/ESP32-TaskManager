// test_stack_size_fix.cpp - Test to verify correct stack size allocation after fix
#include <unity.h>
#include "TaskManager.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

// Test synchronization
static EventGroupHandle_t testEventGroup = nullptr;
static const EventBits_t TASK_STARTED = BIT0;
static const EventBits_t TASK_COMPLETED = BIT1;

// Test results
struct StackTestResults {
    UBaseType_t highWaterMarkWords;
    size_t freeBytes;
    size_t expectedFreeBytes;
    bool testPassed;
};

static StackTestResults testResults = {};

// Test task that uses known amount of stack
void StackTestTask(void* pvParameters) {
    xEventGroupSetBits(testEventGroup, TASK_STARTED);
    
    // Allocate a known amount of stack space
    // We'll use 3000 bytes to ensure we have ~1000 bytes free from a 4096 byte stack
    volatile uint8_t largeArray[3000];
    
    // Fill array to ensure it's actually allocated
    for (int i = 0; i < sizeof(largeArray); i++) {
        largeArray[i] = (uint8_t)(i & 0xFF);
    }
    
    // Force compiler to not optimize away the array
    volatile uint32_t sum = 0;
    for (int i = 0; i < sizeof(largeArray); i++) {
        sum += largeArray[i];
    }
    
    // Get stack high water mark
    testResults.highWaterMarkWords = uxTaskGetStackHighWaterMark(nullptr);
    testResults.freeBytes = testResults.highWaterMarkWords * sizeof(StackType_t);
    
    // Expected: ~1096 bytes free (4096 - 3000)
    // Allow some margin for task overhead
    testResults.expectedFreeBytes = 4096 - 3000;
    
    // Test passes if free bytes is in reasonable range (800-1200 bytes)
    testResults.testPassed = (testResults.freeBytes >= 800 && testResults.freeBytes <= 1200);
    
    xEventGroupSetBits(testEventGroup, TASK_COMPLETED);
    
    // Keep task alive to maintain stack allocation
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// Setup and teardown
void setUp() {
    if (!testEventGroup) {
        testEventGroup = xEventGroupCreate();
    }
    xEventGroupClearBits(testEventGroup, 0xFFFFFF);
    testResults = {};
}

void tearDown() {
    vTaskDelay(pdMS_TO_TICKS(100));
}

// Test cases
void test_stack_size_bytes_not_words() {
    TaskManager tm;
    TaskHandle_t taskHandle = nullptr;
    
    // Create task with 4096 BYTES stack
    bool result = tm.startTask(
        StackTestTask,
        "StackTest",
        4096,  // This should be interpreted as BYTES
        nullptr,
        1,
        "ST",
        &taskHandle
    );
    
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_NOT_NULL(taskHandle);
    
    // Wait for task to complete measurements
    EventBits_t bits = xEventGroupWaitBits(
        testEventGroup,
        TASK_STARTED | TASK_COMPLETED,
        pdTRUE,
        pdTRUE,
        pdMS_TO_TICKS(2000)
    );
    
    TEST_ASSERT_EQUAL(TASK_STARTED | TASK_COMPLETED, bits);
    
    // Verify results
    TEST_ASSERT_TRUE_MESSAGE(testResults.testPassed,
        "Stack allocation incorrect! Expected ~1000 bytes free, but got different amount");
    
    // Log detailed results
    printf("\nStack Size Test Results:\n");
    printf("- Requested stack: 4096 bytes\n");
    printf("- Array allocated: 3000 bytes\n");
    printf("- Free stack found: %u bytes\n", testResults.freeBytes);
    printf("- High water mark: %u words\n", testResults.highWaterMarkWords);
    printf("- Test result: %s\n", testResults.testPassed ? "PASS" : "FAIL");
    
    // Additional assertions
    TEST_ASSERT_GREATER_THAN(800, testResults.freeBytes);
    TEST_ASSERT_LESS_THAN(1200, testResults.freeBytes);
    
    // If we had the old bug, we'd see ~13000 bytes free instead of ~1000
    TEST_ASSERT_LESS_THAN(2000, testResults.freeBytes);
    
    // Cleanup
    vTaskDelete(taskHandle);
}

void test_pinned_task_stack_size() {
    TaskManager tm;
    TaskHandle_t taskHandle = nullptr;
    
    // Test with pinned task too
    bool result = tm.startTaskPinned(
        StackTestTask,
        "PinnedStackTest",
        4096,  // This should be interpreted as BYTES
        nullptr,
        1,
        0,  // Pin to core 0
        "PST",
        &taskHandle
    );
    
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_NOT_NULL(taskHandle);
    
    // Wait for measurements
    EventBits_t bits = xEventGroupWaitBits(
        testEventGroup,
        TASK_COMPLETED,
        pdTRUE,
        pdTRUE,
        pdMS_TO_TICKS(2000)
    );
    
    TEST_ASSERT_NOT_EQUAL(0, bits & TASK_COMPLETED);
    
    // Verify same behavior as unpinned task
    TEST_ASSERT_TRUE(testResults.testPassed);
    TEST_ASSERT_GREATER_THAN(800, testResults.freeBytes);
    TEST_ASSERT_LESS_THAN(1200, testResults.freeBytes);
    
    // Cleanup
    vTaskDelete(taskHandle);
}

void test_minimum_stack_size() {
    TaskManager tm;
    
    // Test minimum stack size (should still be in bytes)
    // configMINIMAL_STACK_SIZE is typically 768 bytes on ESP32
    auto minimalTask = [](void* p) {
        UBaseType_t freeWords = uxTaskGetStackHighWaterMark(nullptr);
        size_t freeBytes = freeWords * sizeof(StackType_t);
        
        // Should have most of the minimal stack free
        TEST_ASSERT_GREATER_THAN(500, freeBytes);
        TEST_ASSERT_LESS_THAN(configMINIMAL_STACK_SIZE, freeBytes);
        
        vTaskDelete(nullptr);
    };
    
    bool result = tm.startTask(
        minimalTask,
        "MinimalStack",
        configMINIMAL_STACK_SIZE,  // Minimum allowed
        nullptr,
        1
    );
    
    TEST_ASSERT_TRUE(result);
    vTaskDelay(pdMS_TO_TICKS(200));
}

// Test runner
void runStackSizeTests() {
    UNITY_BEGIN();
    
    RUN_TEST(test_stack_size_bytes_not_words);
    RUN_TEST(test_pinned_task_stack_size);
    RUN_TEST(test_minimum_stack_size);
    
    UNITY_END();
}