// test_task_recovery.cpp - Unit tests for TaskRecovery functionality
#include <unity.h>
#include "TaskRecovery.h"
#include "TaskManager.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <atomic>

// Test synchronization
static EventGroupHandle_t testEventGroup = nullptr;

// Event bits
#define TEST_TASK_STARTED      BIT0
#define TEST_TASK_CRASHED      BIT1
#define TEST_TASK_RECOVERED    BIT2
#define TEST_TASK_COMPLETED    BIT3
#define TEST_RECOVERY_FAILED   BIT4

// Test context
struct RecoveryTestContext {
    std::atomic<int> runCount{0};
    std::atomic<int> crashCount{0};
    std::atomic<bool> shouldCrash{true};
    TaskHandle_t handle{nullptr};
    TaskRecovery* recovery{nullptr};
};

// Crashing test task
void CrashingTask(void* pvParameters) {
    RecoveryTestContext* ctx = (RecoveryTestContext*)pvParameters;
    ctx->runCount++;
    
    xEventGroupSetBits(testEventGroup, TEST_TASK_STARTED);
    
    if (ctx->shouldCrash && ctx->runCount <= 2) {
        ctx->crashCount++;
        xEventGroupSetBits(testEventGroup, TEST_TASK_CRASHED);
        
        // Simulate crash by divide by zero
        volatile int x = 0;
        volatile int y = 1 / x;
        (void)y;
    }
    
    // If we get here, task is recovered
    xEventGroupSetBits(testEventGroup, TEST_TASK_RECOVERED);
    
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// Stack overflow test task
void StackOverflowTask(void* pvParameters) {
    xEventGroupSetBits(testEventGroup, TEST_TASK_STARTED);
    
    // Intentionally cause stack overflow
    char bigArray[8192]; // Much larger than typical task stack
    memset(bigArray, 0xFF, sizeof(bigArray));
    
    // This should never be reached
    xEventGroupSetBits(testEventGroup, TEST_TASK_COMPLETED);
    vTaskDelete(nullptr);
}

// Memory leak test task
void MemoryLeakTask(void* pvParameters) {
    RecoveryTestContext* ctx = (RecoveryTestContext*)pvParameters;
    
    xEventGroupSetBits(testEventGroup, TEST_TASK_STARTED);
    
    // Allocate memory without freeing
    for (int i = 0; i < 10; i++) {
        void* leak = malloc(1024);
        if (!leak) break;
        // Don't free - intentional leak
    }
    
    ctx->runCount++;
    
    if (ctx->runCount < 3) {
        // Simulate crash to trigger recovery
        xEventGroupSetBits(testEventGroup, TEST_TASK_CRASHED);
        vTaskDelete(nullptr);
    }
    
    xEventGroupSetBits(testEventGroup, TEST_TASK_COMPLETED);
    vTaskDelete(nullptr);
}

// Recovery callback
void recoveryCallback(const char* taskName, RecoveryReason reason, void* userData) {
    RecoveryTestContext* ctx = (RecoveryTestContext*)userData;
    
    if (reason == RecoveryReason::CRASH || reason == RecoveryReason::STACK_OVERFLOW) {
        xEventGroupSetBits(testEventGroup, TEST_TASK_RECOVERED);
    } else {
        xEventGroupSetBits(testEventGroup, TEST_RECOVERY_FAILED);
    }
}

// Test setup and teardown
void setUp() {
    if (!testEventGroup) {
        testEventGroup = xEventGroupCreate();
    }
    xEventGroupClearBits(testEventGroup, 0xFFFFFF);
}

void tearDown() {
    vTaskDelay(pdMS_TO_TICKS(100));
}

// Test cases
void test_recovery_creation() {
    TaskRecovery recovery;
    TEST_ASSERT_TRUE(recovery.isEnabled());
    
    recovery.disable();
    TEST_ASSERT_FALSE(recovery.isEnabled());
    
    recovery.enable();
    TEST_ASSERT_TRUE(recovery.isEnabled());
}

void test_recovery_policy_configuration() {
    TaskRecovery recovery;
    
    // Test default policy
    RecoveryPolicy policy = recovery.getDefaultPolicy();
    TEST_ASSERT_EQUAL(3, policy.maxRetries);
    TEST_ASSERT_EQUAL(1000, policy.retryDelayMs);
    TEST_ASSERT_EQUAL(RecoveryAction::RESTART, policy.action);
    
    // Configure custom policy
    RecoveryPolicy customPolicy{
        .maxRetries = 5,
        .retryDelayMs = 500,
        .backoffMultiplier = 2.0f,
        .maxDelayMs = 10000,
        .action = RecoveryAction::RESTART
    };
    
    recovery.configurePolicy("TestTask", customPolicy);
    
    // Verify policy was set
    const auto* retrieved = recovery.getPolicy("TestTask");
    TEST_ASSERT_NOT_NULL(retrieved);
    TEST_ASSERT_EQUAL(5, retrieved->maxRetries);
    TEST_ASSERT_EQUAL(500, retrieved->retryDelayMs);
}

void test_crash_detection() {
    TaskRecovery recovery;
    RecoveryTestContext ctx;
    ctx.recovery = &recovery;
    
    // Configure recovery policy
    RecoveryPolicy policy{
        .maxRetries = 3,
        .retryDelayMs = 100,
        .backoffMultiplier = 1.0f,
        .maxDelayMs = 1000,
        .action = RecoveryAction::RESTART
    };
    
    recovery.configurePolicy("CrashTask", policy);
    recovery.setRecoveryCallback(recoveryCallback, &ctx);
    
    // Register task for monitoring
    TaskHandle_t handle = xTaskCreateStatic(
        CrashingTask,
        "CrashTask",
        4096,
        &ctx,
        1,
        (StackType_t*)malloc(4096),
        (StaticTask_t*)malloc(sizeof(StaticTask_t))
    );
    
    ctx.handle = handle;
    recovery.registerTask(handle, "CrashTask");
    
    // Monitor for crash
    recovery.monitorTask(handle);
    
    // Wait for crash and recovery
    EventBits_t bits = xEventGroupWaitBits(
        testEventGroup,
        TEST_TASK_CRASHED | TEST_TASK_RECOVERED,
        pdTRUE,
        pdTRUE,
        pdMS_TO_TICKS(5000)
    );
    
    TEST_ASSERT_NOT_EQUAL(0, bits & TEST_TASK_CRASHED);
    TEST_ASSERT_GREATER_THAN(0, ctx.crashCount.load());
    
    // Cleanup
    if (ctx.handle) {
        vTaskDelete(ctx.handle);
    }
}

void test_retry_limit() {
    TaskRecovery recovery;
    RecoveryTestContext ctx;
    
    // Configure with limited retries
    RecoveryPolicy policy{
        .maxRetries = 2,
        .retryDelayMs = 50,
        .backoffMultiplier = 1.0f,
        .maxDelayMs = 1000,
        .action = RecoveryAction::RESTART
    };
    
    recovery.configurePolicy("LimitTask", policy);
    
    // Track recovery attempts
    int recoveryAttempts = 0;
    recovery.setRecoveryCallback([](const char* name, RecoveryReason reason, void* data) {
        int* attempts = (int*)data;
        (*attempts)++;
    }, &recoveryAttempts);
    
    // Create a task that always crashes
    ctx.shouldCrash = true;
    TaskHandle_t handle = xTaskCreate(
        CrashingTask,
        "LimitTask",
        4096,
        &ctx,
        1,
        &handle
    );
    
    recovery.registerTask(handle, "LimitTask");
    
    // Wait for retries to exhaust
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // Should have attempted recovery up to the limit
    TEST_ASSERT_LESS_OR_EQUAL(policy.maxRetries, recoveryAttempts);
    
    // Cleanup
    if (handle && eTaskGetState(handle) != eDeleted) {
        vTaskDelete(handle);
    }
}

void test_memory_tracking() {
    TaskRecovery recovery;
    
    // Get baseline memory
    size_t baseline = recovery.getMemoryUsage("MemTask");
    TEST_ASSERT_EQUAL(0, baseline);
    
    // Create task and track memory
    RecoveryTestContext ctx;
    TaskHandle_t handle = xTaskCreate(
        MemoryLeakTask,
        "MemTask",
        4096,
        &ctx,
        1,
        &handle
    );
    
    recovery.registerTask(handle, "MemTask");
    recovery.trackMemory(handle, "MemTask");
    
    // Let task run and allocate memory
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // Check memory usage increased
    size_t usage = recovery.getMemoryUsage("MemTask");
    TEST_ASSERT_GREATER_THAN(baseline, usage);
    
    // Cleanup
    if (handle && eTaskGetState(handle) != eDeleted) {
        vTaskDelete(handle);
    }
}

void test_recovery_statistics() {
    TaskRecovery recovery;
    
    // Get initial stats
    RecoveryStats stats = recovery.getStatistics();
    size_t initialRecoveries = stats.totalRecoveries;
    size_t initialFailures = stats.totalFailures;
    
    // Configure and create crashing task
    RecoveryTestContext ctx;
    ctx.shouldCrash = true;
    ctx.runCount = 0;
    
    RecoveryPolicy policy{
        .maxRetries = 1,
        .retryDelayMs = 100,
        .backoffMultiplier = 1.0f,
        .maxDelayMs = 1000,
        .action = RecoveryAction::RESTART
    };
    
    recovery.configurePolicy("StatsTask", policy);
    
    TaskHandle_t handle = xTaskCreate(
        CrashingTask,
        "StatsTask", 
        4096,
        &ctx,
        1,
        &handle
    );
    
    recovery.registerTask(handle, "StatsTask");
    recovery.monitorTask(handle);
    
    // Wait for crash and recovery attempt
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // Check updated stats
    stats = recovery.getStatistics();
    TEST_ASSERT_GREATER_THAN(initialRecoveries, stats.totalRecoveries);
    
    // Get task-specific stats
    auto taskStats = recovery.getTaskStatistics("StatsTask");
    TEST_ASSERT_NOT_NULL(taskStats);
    TEST_ASSERT_GREATER_THAN(0, taskStats->recoveryCount);
    
    // Cleanup
    if (handle && eTaskGetState(handle) != eDeleted) {
        vTaskDelete(handle);
    }
}

void test_recovery_callback() {
    TaskRecovery recovery;
    RecoveryTestContext ctx;
    bool callbackCalled = false;
    RecoveryReason capturedReason = RecoveryReason::UNKNOWN;
    
    // Set recovery callback
    recovery.setRecoveryCallback([](const char* name, RecoveryReason reason, void* data) {
        bool* called = (bool*)data;
        *called = true;
    }, &callbackCalled);
    
    // Configure recovery
    RecoveryPolicy policy{
        .maxRetries = 1,
        .retryDelayMs = 50,
        .backoffMultiplier = 1.0f,
        .maxDelayMs = 1000,
        .action = RecoveryAction::RESTART
    };
    
    recovery.configurePolicy("CallbackTask", policy);
    
    // Create crashing task
    TaskHandle_t handle = xTaskCreate(
        CrashingTask,
        "CallbackTask",
        4096,
        &ctx,
        1,
        &handle
    );
    
    recovery.registerTask(handle, "CallbackTask");
    recovery.monitorTask(handle);
    
    // Wait for crash and callback
    vTaskDelay(pdMS_TO_TICKS(500));
    
    TEST_ASSERT_TRUE(callbackCalled);
    
    // Cleanup
    if (handle && eTaskGetState(handle) != eDeleted) {
        vTaskDelete(handle);
    }
}

void test_cleanup_on_delete() {
    TaskRecovery recovery;
    
    // Create and register task
    TaskHandle_t handle = xTaskCreate(
        [](void* p) { 
            vTaskDelay(pdMS_TO_TICKS(100));
            vTaskDelete(nullptr);
        },
        "CleanupTask",
        4096,
        nullptr,
        1,
        &handle
    );
    
    recovery.registerTask(handle, "CleanupTask");
    
    // Verify task is registered
    auto stats = recovery.getTaskStatistics("CleanupTask");
    TEST_ASSERT_NOT_NULL(stats);
    
    // Wait for task to delete itself
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // Cleanup deleted tasks
    recovery.cleanupDeletedTasks();
    
    // Verify task was cleaned up
    stats = recovery.getTaskStatistics("CleanupTask");
    TEST_ASSERT_NULL(stats);
}

// Test runner
void runTaskRecoveryTests() {
    UNITY_BEGIN();
    
    RUN_TEST(test_recovery_creation);
    RUN_TEST(test_recovery_policy_configuration);
    RUN_TEST(test_crash_detection);
    RUN_TEST(test_retry_limit);
    RUN_TEST(test_memory_tracking);
    RUN_TEST(test_recovery_statistics);
    RUN_TEST(test_recovery_callback);
    RUN_TEST(test_cleanup_on_delete);
    
    UNITY_END();
}