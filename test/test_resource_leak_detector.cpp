// test_resource_leak_detector.cpp - Unit tests for ResourceLeakDetector
#include <unity.h>
#include "ResourceLeakDetector.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <vector>

// Test context
struct LeakTestContext {
    std::vector<void*> allocations;
    std::vector<SemaphoreHandle_t> semaphores;
    std::vector<QueueHandle_t> queues;
    bool leakDetected;
    const char* leakType;
    size_t leakSize;
};

// Leak detection callback
void leakCallback(const char* taskName, const char* resourceType, size_t size, void* userData) {
    LeakTestContext* ctx = (LeakTestContext*)userData;
    ctx->leakDetected = true;
    ctx->leakType = resourceType;
    ctx->leakSize = size;
}

// Test task that allocates memory
void MemoryAllocTask(void* pvParameters) {
    LeakTestContext* ctx = (LeakTestContext*)pvParameters;
    
    // Allocate some memory
    for (int i = 0; i < 5; i++) {
        void* mem = malloc(256);
        if (mem) {
            ctx->allocations.push_back(mem);
        }
    }
    
    // Free some but not all (create leak)
    for (int i = 0; i < 3; i++) {
        if (i < ctx->allocations.size()) {
            free(ctx->allocations[i]);
            ctx->allocations[i] = nullptr;
        }
    }
    
    vTaskDelay(pdMS_TO_TICKS(100));
    vTaskDelete(nullptr);
}

// Test task that creates semaphores
void SemaphoreLeakTask(void* pvParameters) {
    LeakTestContext* ctx = (LeakTestContext*)pvParameters;
    
    // Create semaphores
    for (int i = 0; i < 3; i++) {
        SemaphoreHandle_t sem = xSemaphoreCreateBinary();
        if (sem) {
            ctx->semaphores.push_back(sem);
        }
    }
    
    // Delete only first one (leak the rest)
    if (!ctx->semaphores.empty()) {
        vSemaphoreDelete(ctx->semaphores[0]);
        ctx->semaphores[0] = nullptr;
    }
    
    vTaskDelay(pdMS_TO_TICKS(100));
    vTaskDelete(nullptr);
}

// Test task that creates queues
void QueueLeakTask(void* pvParameters) {
    LeakTestContext* ctx = (LeakTestContext*)pvParameters;
    
    // Create queues
    for (int i = 0; i < 2; i++) {
        QueueHandle_t queue = xQueueCreate(10, sizeof(int));
        if (queue) {
            ctx->queues.push_back(queue);
        }
    }
    
    // Don't delete any (leak all)
    
    vTaskDelay(pdMS_TO_TICKS(100));
    vTaskDelete(nullptr);
}

// Test setup and teardown
void setUp() {
    // Nothing specific needed
}

void tearDown() {
    vTaskDelay(pdMS_TO_TICKS(100));
}

// Test cases
void test_detector_creation() {
    ResourceLeakDetector detector;
    TEST_ASSERT_TRUE(detector.isEnabled());
    
    detector.disable();
    TEST_ASSERT_FALSE(detector.isEnabled());
    
    detector.enable();
    TEST_ASSERT_TRUE(detector.isEnabled());
}

void test_memory_leak_detection() {
    ResourceLeakDetector detector;
    LeakTestContext ctx = {};
    
    // Set callback
    detector.setLeakCallback(leakCallback, &ctx);
    
    // Create task that will leak memory
    TaskHandle_t handle = xTaskCreate(
        MemoryAllocTask,
        "MemLeakTask",
        4096,
        &ctx,
        1,
        &handle
    );
    
    // Start tracking
    detector.startTracking(handle, "MemLeakTask");
    
    // Wait for task to complete
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // Stop tracking and check for leaks
    detector.stopTracking(handle);
    auto report = detector.checkForLeaks("MemLeakTask");
    
    TEST_ASSERT_TRUE(report.hasLeaks);
    TEST_ASSERT_GREATER_THAN(0, report.memoryLeaked);
    TEST_ASSERT_EQUAL(2, report.unfreedAllocations); // 5 allocated, 3 freed
    
    // Cleanup remaining allocations
    for (auto ptr : ctx.allocations) {
        if (ptr) free(ptr);
    }
}

void test_semaphore_leak_detection() {
    ResourceLeakDetector detector;
    LeakTestContext ctx = {};
    
    detector.setLeakCallback(leakCallback, &ctx);
    
    // Create task that will leak semaphores
    TaskHandle_t handle = xTaskCreate(
        SemaphoreLeakTask,
        "SemLeakTask",
        4096,
        &ctx,
        1,
        &handle
    );
    
    detector.startTracking(handle, "SemLeakTask");
    
    // Wait for task
    vTaskDelay(pdMS_TO_TICKS(200));
    
    detector.stopTracking(handle);
    auto report = detector.checkForLeaks("SemLeakTask");
    
    TEST_ASSERT_TRUE(report.hasLeaks);
    TEST_ASSERT_EQUAL(2, report.semaphoresLeaked); // 3 created, 1 deleted
    
    // Cleanup
    for (auto sem : ctx.semaphores) {
        if (sem) vSemaphoreDelete(sem);
    }
}

void test_queue_leak_detection() {
    ResourceLeakDetector detector;
    LeakTestContext ctx = {};
    
    detector.setLeakCallback(leakCallback, &ctx);
    
    // Create task that will leak queues
    TaskHandle_t handle = xTaskCreate(
        QueueLeakTask,
        "QueueLeakTask",
        4096,
        &ctx,
        1,
        &handle
    );
    
    detector.startTracking(handle, "QueueLeakTask");
    
    vTaskDelay(pdMS_TO_TICKS(200));
    
    detector.stopTracking(handle);
    auto report = detector.checkForLeaks("QueueLeakTask");
    
    TEST_ASSERT_TRUE(report.hasLeaks);
    TEST_ASSERT_EQUAL(2, report.queuesLeaked);
    
    // Cleanup
    for (auto queue : ctx.queues) {
        if (queue) vQueueDelete(queue);
    }
}

void test_no_leak_scenario() {
    ResourceLeakDetector detector;
    
    // Task that properly cleans up
    auto cleanTask = [](void* p) {
        void* mem = malloc(128);
        SemaphoreHandle_t sem = xSemaphoreCreateBinary();
        QueueHandle_t queue = xQueueCreate(5, sizeof(int));
        
        vTaskDelay(pdMS_TO_TICKS(50));
        
        // Clean up everything
        free(mem);
        vSemaphoreDelete(sem);
        vQueueDelete(queue);
        
        vTaskDelete(nullptr);
    };
    
    TaskHandle_t handle = xTaskCreate(
        cleanTask,
        "CleanTask",
        4096,
        nullptr,
        1,
        &handle
    );
    
    detector.startTracking(handle, "CleanTask");
    
    vTaskDelay(pdMS_TO_TICKS(200));
    
    detector.stopTracking(handle);
    auto report = detector.checkForLeaks("CleanTask");
    
    TEST_ASSERT_FALSE(report.hasLeaks);
    TEST_ASSERT_EQUAL(0, report.memoryLeaked);
    TEST_ASSERT_EQUAL(0, report.unfreedAllocations);
    TEST_ASSERT_EQUAL(0, report.semaphoresLeaked);
    TEST_ASSERT_EQUAL(0, report.queuesLeaked);
}

void test_threshold_configuration() {
    ResourceLeakDetector detector;
    
    // Configure thresholds
    detector.setMemoryThreshold(1024);  // 1KB threshold
    detector.setSemaphoreThreshold(5);
    detector.setQueueThreshold(3);
    
    // Test that small leaks don't trigger
    auto smallLeakTask = [](void* p) {
        malloc(512); // Below threshold
        xSemaphoreCreateBinary(); // 1 semaphore (below threshold)
        vTaskDelay(pdMS_TO_TICKS(50));
        vTaskDelete(nullptr);
    };
    
    TaskHandle_t handle = xTaskCreate(
        smallLeakTask,
        "SmallLeakTask",
        4096,
        nullptr,
        1,
        &handle
    );
    
    detector.startTracking(handle, "SmallLeakTask");
    vTaskDelay(pdMS_TO_TICKS(200));
    detector.stopTracking(handle);
    
    auto report = detector.checkForLeaks("SmallLeakTask");
    
    // Should detect leaks but they're below threshold
    TEST_ASSERT_TRUE(report.hasLeaks);
    TEST_ASSERT_LESS_THAN(1024, report.memoryLeaked);
}

void test_leak_callback_invocation() {
    ResourceLeakDetector detector;
    LeakTestContext ctx = {};
    ctx.leakDetected = false;
    
    detector.setLeakCallback(leakCallback, &ctx);
    
    // Task that leaks
    auto leakTask = [](void* p) {
        malloc(512);
        malloc(256);
        vTaskDelay(pdMS_TO_TICKS(50));
        vTaskDelete(nullptr);
    };
    
    TaskHandle_t handle = xTaskCreate(
        leakTask,
        "CallbackTask",
        4096,
        nullptr,
        1,
        &handle
    );
    
    detector.startTracking(handle, "CallbackTask");
    vTaskDelay(pdMS_TO_TICKS(200));
    detector.stopTracking(handle);
    
    // Check for leaks should trigger callback
    detector.checkForLeaks("CallbackTask");
    
    TEST_ASSERT_TRUE(ctx.leakDetected);
    TEST_ASSERT_EQUAL_STRING("memory", ctx.leakType);
    TEST_ASSERT_EQUAL(768, ctx.leakSize); // 512 + 256
}

void test_statistics_tracking() {
    ResourceLeakDetector detector;
    
    // Get initial stats
    auto stats = detector.getStatistics();
    size_t initialChecks = stats.totalChecks;
    size_t initialLeaks = stats.totalLeaksDetected;
    
    // Create leaking task
    auto leakTask = [](void* p) {
        malloc(1024);
        xSemaphoreCreateBinary();
        vTaskDelay(pdMS_TO_TICKS(50));
        vTaskDelete(nullptr);
    };
    
    TaskHandle_t handle = xTaskCreate(
        leakTask,
        "StatsTask",
        4096,
        nullptr,
        1,
        &handle
    );
    
    detector.startTracking(handle, "StatsTask");
    vTaskDelay(pdMS_TO_TICKS(200));
    detector.stopTracking(handle);
    detector.checkForLeaks("StatsTask");
    
    // Check updated stats
    stats = detector.getStatistics();
    TEST_ASSERT_GREATER_THAN(initialChecks, stats.totalChecks);
    TEST_ASSERT_GREATER_THAN(initialLeaks, stats.totalLeaksDetected);
    TEST_ASSERT_GREATER_THAN(0, stats.totalMemoryLeaked);
    TEST_ASSERT_GREATER_THAN(0, stats.totalSemaphoresLeaked);
}

void test_multiple_task_tracking() {
    ResourceLeakDetector detector;
    
    // Create multiple tasks with different leak patterns
    std::vector<TaskHandle_t> handles;
    
    for (int i = 0; i < 3; i++) {
        char name[32];
        snprintf(name, sizeof(name), "MultiTask%d", i);
        
        auto task = [](void* p) {
            int id = *(int*)p;
            
            // Different leak patterns for each task
            for (int j = 0; j <= id; j++) {
                malloc(128);
            }
            
            vTaskDelay(pdMS_TO_TICKS(50));
            vTaskDelete(nullptr);
        };
        
        TaskHandle_t handle;
        handles.push_back(xTaskCreate(
            task,
            name,
            4096,
            &i,
            1,
            &handle
        ));
        
        detector.startTracking(handle, name);
    }
    
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // Check each task
    for (int i = 0; i < 3; i++) {
        char name[32];
        snprintf(name, sizeof(name), "MultiTask%d", i);
        
        detector.stopTracking(handles[i]);
        auto report = detector.checkForLeaks(name);
        
        TEST_ASSERT_TRUE(report.hasLeaks);
        TEST_ASSERT_EQUAL((i + 1) * 128, report.memoryLeaked);
    }
}

void test_cleanup_deleted_tasks() {
    ResourceLeakDetector detector;
    
    // Track tasks that will delete themselves
    for (int i = 0; i < 3; i++) {
        char name[32];
        snprintf(name, sizeof(name), "CleanupTask%d", i);
        
        auto task = [](void* p) {
            vTaskDelay(pdMS_TO_TICKS(50));
            vTaskDelete(nullptr);
        };
        
        TaskHandle_t handle = xTaskCreate(
            task,
            name,
            4096,
            nullptr,
            1,
            &handle
        );
        
        detector.startTracking(handle, name);
    }
    
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // Cleanup should remove tracking for deleted tasks
    detector.cleanupDeletedTasks();
    
    // Verify cleanup worked by checking statistics
    auto stats = detector.getStatistics();
    TEST_ASSERT_EQUAL(0, stats.currentTrackedTasks);
}

// Test runner
void runResourceLeakDetectorTests() {
    UNITY_BEGIN();
    
    RUN_TEST(test_detector_creation);
    RUN_TEST(test_memory_leak_detection);
    RUN_TEST(test_semaphore_leak_detection);
    RUN_TEST(test_queue_leak_detection);
    RUN_TEST(test_no_leak_scenario);
    RUN_TEST(test_threshold_configuration);
    RUN_TEST(test_leak_callback_invocation);
    RUN_TEST(test_statistics_tracking);
    RUN_TEST(test_multiple_task_tracking);
    RUN_TEST(test_cleanup_deleted_tasks);
    
    UNITY_END();
}