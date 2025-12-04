// test_taskmanager_core.cpp - Core unit tests for TaskManager library
#include <unity.h>
#include "TaskManager.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>

// Test synchronization
static EventGroupHandle_t testEventGroup = nullptr;
static SemaphoreHandle_t testMutex = nullptr;

// Event bits
#define TEST_TASK_STARTED    BIT0
#define TEST_TASK_COMPLETED  BIT1
#define TEST_TASK_FAILED     BIT2
#define TEST_WATCHDOG_FED    BIT3

// Test state
struct TestContext {
    int counter;
    bool success;
    TaskHandle_t handle;
    char message[64];
};

// Simple test task
void SimpleTestTask(void* pvParameters) {
    TestContext* ctx = (TestContext*)pvParameters;
    if (ctx) {
        ctx->counter++;
        ctx->success = true;
    }
    xEventGroupSetBits(testEventGroup, TEST_TASK_STARTED);
    vTaskDelay(pdMS_TO_TICKS(50));
    xEventGroupSetBits(testEventGroup, TEST_TASK_COMPLETED);
    vTaskDelete(nullptr);
}

// Long running task
void LongRunningTask(void* pvParameters) {
    xEventGroupSetBits(testEventGroup, TEST_TASK_STARTED);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// Watchdog test task
void WatchdogTestTask(void* pvParameters) {
    TaskManager* tm = (TaskManager*)pvParameters;
    xEventGroupSetBits(testEventGroup, TEST_TASK_STARTED);
    
    // Register with watchdog
    TaskManager::WatchdogConfig config = TaskManager::WatchdogConfig::enabled(false, 1000);
    if (tm->registerCurrentTaskWithWatchdog("WDTest", config)) {
        // Feed watchdog periodically
        for (int i = 0; i < 5; i++) {
            vTaskDelay(pdMS_TO_TICKS(200));
            tm->feedWatchdog();
            xEventGroupSetBits(testEventGroup, TEST_WATCHDOG_FED);
        }
        tm->unregisterCurrentTaskFromWatchdog();
    }
    
    xEventGroupSetBits(testEventGroup, TEST_TASK_COMPLETED);
    vTaskDelete(nullptr);
}

// Test setup and teardown
void setUp() {
    if (!testEventGroup) {
        testEventGroup = xEventGroupCreate();
    }
    if (!testMutex) {
        testMutex = xSemaphoreCreateMutex();
    }
    xEventGroupClearBits(testEventGroup, 0xFFFFFF);
}

void tearDown() {
    vTaskDelay(pdMS_TO_TICKS(100));
}

// Test cases
void test_taskmanager_creation() {
    TaskManager tm;
    TEST_ASSERT_EQUAL(0, tm.getTaskList().size());
}

void test_start_simple_task() {
    TaskManager tm;
    TestContext ctx = {0, false, nullptr, ""};
    
    bool result = tm.startTask(
        SimpleTestTask,
        "SimpleTask",
        4096,
        &ctx,
        1,
        "ST"
    );
    
    TEST_ASSERT_TRUE(result);
    
    EventBits_t bits = xEventGroupWaitBits(
        testEventGroup,
        TEST_TASK_STARTED | TEST_TASK_COMPLETED,
        pdTRUE,
        pdTRUE,
        pdMS_TO_TICKS(1000)
    );
    
    TEST_ASSERT_EQUAL(TEST_TASK_STARTED | TEST_TASK_COMPLETED, bits);
    TEST_ASSERT_EQUAL(1, ctx.counter);
    TEST_ASSERT_TRUE(ctx.success);
}

void test_start_pinned_task() {
    TaskManager tm;
    
    bool result = tm.startTaskPinned(
        SimpleTestTask,
        "PinnedTask",
        4096,
        nullptr,
        1,
        0,  // Core 0
        "PT"
    );
    
    TEST_ASSERT_TRUE(result);
    
    xEventGroupWaitBits(testEventGroup, TEST_TASK_STARTED, pdTRUE, pdTRUE, pdMS_TO_TICKS(1000));
    
    const auto& tasks = tm.getTaskList();
    TEST_ASSERT_EQUAL(1, tasks.size());
    TEST_ASSERT_TRUE(tasks[0].isPinned);
    TEST_ASSERT_EQUAL(0, tasks[0].coreID);
}

void test_task_handle_retrieval() {
    TaskManager tm;
    TaskHandle_t handle = nullptr;
    
    bool result = tm.startTask(
        LongRunningTask,
        "HandleTest",
        4096,
        nullptr,
        1,
        "HT",
        &handle
    );
    
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_NOT_NULL(handle);
    
    xEventGroupWaitBits(testEventGroup, TEST_TASK_STARTED, pdTRUE, pdTRUE, pdMS_TO_TICKS(1000));
    
    TaskHandle_t retrieved = tm.getTaskHandleByName("HandleTest");
    TEST_ASSERT_EQUAL(handle, retrieved);
    
    // Cleanup
    vTaskDelete(handle);
}

void test_invalid_task_parameters() {
    TaskManager tm;
    
    // Null function
    TEST_ASSERT_FALSE(tm.startTask(nullptr, "NullFunc", 4096, nullptr, 1));
    
    // Empty name
    TEST_ASSERT_FALSE(tm.startTask(SimpleTestTask, "", 4096, nullptr, 1));
    
    // Zero stack size
    TEST_ASSERT_FALSE(tm.startTask(SimpleTestTask, "ZeroStack", 0, nullptr, 1));
    
    // Invalid priority
    TEST_ASSERT_FALSE(tm.startTask(SimpleTestTask, "BadPrio", 4096, nullptr, configMAX_PRIORITIES));
}

void test_task_names() {
    TaskManager tm;
    TaskManager::TaskNames names("VeryLongTaskNameThatExceedsLimit", "LONG", "LongTag");
    
    bool result = tm.startTask(
        SimpleTestTask,
        names,
        4096,
        nullptr,
        1
    );
    
    TEST_ASSERT_TRUE(result);
    
    xEventGroupWaitBits(testEventGroup, TEST_TASK_COMPLETED, pdTRUE, pdTRUE, pdMS_TO_TICKS(1000));
    
    // Check name handling
    std::string freertosName = tm.getFreeRTOSNameByFullName(names.fullName);
    std::string logTag = tm.getLogTagByFullName(names.fullName);
    
    TEST_ASSERT_TRUE(freertosName.length() < configMAX_TASK_NAME_LEN);
    TEST_ASSERT_EQUAL_STRING("LongTag", logTag.c_str());
}

void test_task_statistics() {
    TaskManager tm;
    
    // Create multiple tasks
    for (int i = 0; i < 3; i++) {
        char name[32];
        snprintf(name, sizeof(name), "StatTask%d", i);
        tm.startTask(SimpleTestTask, name, 4096, nullptr, 1);
    }
    
    vTaskDelay(pdMS_TO_TICKS(200));
    
    TaskManager::TaskStatistics stats = tm.getTaskStatistics();
    TEST_ASSERT_GREATER_OR_EQUAL(3, stats.totalTasks);
}

void test_watchdog_basic() {
    TaskManager tm;
    
    // Initialize watchdog
    TEST_ASSERT_TRUE(tm.initWatchdog(30, true));
    TEST_ASSERT_TRUE(tm.isWatchdogInitialized());
}

void test_watchdog_task_registration() {
    TaskManager tm;
    
    // Initialize watchdog
    TEST_ASSERT_TRUE(tm.initWatchdog(30, true));
    
    // Start task with watchdog
    TaskManager::WatchdogConfig config = TaskManager::WatchdogConfig::enabled(false, 2000);
    
    bool result = tm.startTask(
        WatchdogTestTask,
        "WatchdogTask",
        4096,
        &tm,
        1,
        "WDT",
        nullptr,
        config
    );
    
    TEST_ASSERT_TRUE(result);
    
    EventBits_t bits = xEventGroupWaitBits(
        testEventGroup,
        TEST_TASK_COMPLETED | TEST_WATCHDOG_FED,
        pdTRUE,
        pdTRUE,
        pdMS_TO_TICKS(2000)
    );
    
    TEST_ASSERT_NOT_EQUAL(0, bits & TEST_WATCHDOG_FED);
    TEST_ASSERT_NOT_EQUAL(0, bits & TEST_TASK_COMPLETED);
}

void test_cleanup_deleted_tasks() {
    TaskManager tm;
    
    // Create tasks that will self-delete
    for (int i = 0; i < 5; i++) {
        char name[32];
        snprintf(name, sizeof(name), "CleanupTask%d", i);
        tm.startTask(SimpleTestTask, name, 4096, nullptr, 1);
    }
    
    // Wait for tasks to complete and delete
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // Cleanup should remove deleted tasks
    tm.cleanupDeletedTasks();
    
    // Verify no invalid tasks remain
    const auto& tasks = tm.getTaskList();
    for (const auto& task : tasks) {
        TEST_ASSERT_TRUE(tm.isTaskValid(task.taskHandle));
    }
}

void test_log_tag_management() {
    TaskManager tm;
    
    TaskManager::TaskNames names1("Task1", "T1", "Tag1");
    TaskManager::TaskNames names2("Task2", "T2", "Tag2");
    
    tm.startTask(SimpleTestTask, names1, 4096, nullptr, 1);
    tm.startTask(SimpleTestTask, names2, 4096, nullptr, 1);
    
    vTaskDelay(pdMS_TO_TICKS(200));
    
    std::vector<std::string> tags = tm.getAllLogTags();
    TEST_ASSERT_GREATER_OR_EQUAL(2, tags.size());
    
    // Test log level configuration
    tm.configureTaskLogLevel("Task1", ESP_LOG_DEBUG);
    tm.configureTaskGroupLogLevel("Tag", ESP_LOG_INFO);
}

// Test runner
void runTaskManagerTests() {
    UNITY_BEGIN();
    
    RUN_TEST(test_taskmanager_creation);
    RUN_TEST(test_start_simple_task);
    RUN_TEST(test_start_pinned_task);
    RUN_TEST(test_task_handle_retrieval);
    RUN_TEST(test_invalid_task_parameters);
    RUN_TEST(test_task_names);
    RUN_TEST(test_task_statistics);
    RUN_TEST(test_watchdog_basic);
    RUN_TEST(test_watchdog_task_registration);
    RUN_TEST(test_cleanup_deleted_tasks);
    RUN_TEST(test_log_tag_management);
    
    UNITY_END();
}