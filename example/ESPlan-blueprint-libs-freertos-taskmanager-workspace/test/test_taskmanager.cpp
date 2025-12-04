// test_taskmanager.cpp - Comprehensive unit tests for TaskManager
#include <Arduino.h>
#include <unity.h>
#include <TaskManager.h>
#include <Logger.h>
#include <ConsoleBackend.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/event_groups.h>

// Global test instances
Logger testLogger(std::make_shared<ConsoleBackend>());
Logger logger(std::make_shared<ConsoleBackend>());  // Required by TaskManager
TaskManager* testTaskManager = nullptr;

// Test synchronization
EventGroupHandle_t testEventGroup = nullptr;
SemaphoreHandle_t testMutex = nullptr;

// Event bits for test synchronization
#define TEST_TASK_STARTED          BIT0
#define TEST_TASK_COMPLETED        BIT1
#define TEST_WATCHDOG_TRIGGERED    BIT2
#define TEST_TASK_DELETED          BIT3
#define TEST_STATE_CHANGED         BIT4

// Test state tracking
struct TestState {
    bool taskStarted;
    bool taskCompleted;
    bool watchdogFed;
    uint32_t counter;
    TaskHandle_t handle;
    eTaskState lastState;
} testState = {false, false, false, 0, nullptr, eInvalid};

// Simple test task function
void SimpleTestTask(void* pvParameters) {
    int* value = (int*)pvParameters;
    if (value) {
        (*value)++;
    }
    xEventGroupSetBits(testEventGroup, TEST_TASK_STARTED);
    vTaskDelay(pdMS_TO_TICKS(100));
    xEventGroupSetBits(testEventGroup, TEST_TASK_COMPLETED);
    vTaskDelete(nullptr);
}

// Member function test task
class TestTaskClass {
public:
    void memberTaskFunction() {
        testState.taskStarted = true;
        xEventGroupSetBits(testEventGroup, TEST_TASK_STARTED);
        vTaskDelay(pdMS_TO_TICKS(100));
        testState.taskCompleted = true;
        xEventGroupSetBits(testEventGroup, TEST_TASK_COMPLETED);
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
};

// Watchdog test task
void WatchdogTestTask(void* pvParameters) {
    xEventGroupSetBits(testEventGroup, TEST_TASK_STARTED);
    
    // Register with watchdog
    TaskManager::WatchdogConfig config = TaskManager::WatchdogConfig::enabled(false, 1000);
    testTaskManager->registerCurrentTaskWithWatchdog("WatchdogTest", config);
    
    // Feed watchdog a few times
    for (int i = 0; i < 3; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        testTaskManager->feedWatchdog();
        testState.watchdogFed = true;
    }
    
    xEventGroupSetBits(testEventGroup, TEST_TASK_COMPLETED);
    
    // Unregister before deleting
    testTaskManager->unregisterCurrentTaskFromWatchdog();
    vTaskDelete(nullptr);
}

// State monitoring test task
void StateMonitorTestTask(void* pvParameters) {
    testState.lastState = eRunning;
    xEventGroupSetBits(testEventGroup, TEST_TASK_STARTED);
    
    // Change states
    vTaskDelay(pdMS_TO_TICKS(100)); // Running -> Blocked
    testState.lastState = eBlocked;
    
    vTaskSuspend(nullptr); // Blocked -> Suspended
    testState.lastState = eSuspended;
    
    // Task will be resumed by test
    xEventGroupSetBits(testEventGroup, TEST_STATE_CHANGED);
    vTaskDelete(nullptr);
}

// ===== Setup and Teardown =====

void setUp() {
    // Create fresh TaskManager instance for each test
    testTaskManager = new TaskManager();
    
    // Create synchronization primitives
    if (!testEventGroup) {
        testEventGroup = xEventGroupCreate();
    }
    if (!testMutex) {
        testMutex = xSemaphoreCreateMutex();
    }
    
    // Clear event bits
    xEventGroupClearBits(testEventGroup, 0xFFFFFF);
    
    // Reset test state
    testState = {false, false, false, 0, nullptr, eInvalid};
}

void tearDown() {
    // Clean up any remaining tasks
    if (testState.handle) {
        vTaskDelete(testState.handle);
        testState.handle = nullptr;
    }
    
    // Delete TaskManager instance
    delete testTaskManager;
    testTaskManager = nullptr;
    
    // Small delay for cleanup
    vTaskDelay(pdMS_TO_TICKS(100));
}

// ===== Test Cases =====

void test_TaskManager_Construction() {
    TEST_ASSERT_NOT_NULL(testTaskManager);
    TEST_ASSERT_EQUAL(0, testTaskManager->getTaskList().size());
}

void test_StartSimpleTask() {
    int testValue = 0;
    
    bool result = testTaskManager->startTask(
        SimpleTestTask, 
        "SimpleTest", 
        4096, 
        &testValue, 
        1,
        "ST"
    );
    
    TEST_ASSERT_TRUE(result);
    
    // Wait for task to start
    EventBits_t bits = xEventGroupWaitBits(
        testEventGroup, 
        TEST_TASK_STARTED, 
        pdTRUE, 
        pdTRUE, 
        pdMS_TO_TICKS(1000)
    );
    
    TEST_ASSERT_EQUAL(TEST_TASK_STARTED, bits & TEST_TASK_STARTED);
    TEST_ASSERT_EQUAL(1, testValue);
    
    // Verify task is in list
    TEST_ASSERT_EQUAL(1, testTaskManager->getTaskList().size());
}

void test_StartTaskWithNames() {
    TaskManager::TaskNames names("VeryLongTaskNameThatExceedsFreeRTOSLimit", "LONG", "LongTag");
    
    bool result = testTaskManager->startTask(
        SimpleTestTask,
        names,
        4096,
        nullptr,
        1
    );
    
    TEST_ASSERT_TRUE(result);
    
    // Wait for task to start
    xEventGroupWaitBits(testEventGroup, TEST_TASK_STARTED, pdTRUE, pdTRUE, pdMS_TO_TICKS(1000));
    
    // Check name handling
    std::string freertosName = testTaskManager->getFreeRTOSNameByFullName(names.fullName);
    std::string logTag = testTaskManager->getLogTagByFullName(names.fullName);
    
    TEST_ASSERT_TRUE(freertosName.length() < configMAX_TASK_NAME_LEN);
    TEST_ASSERT_EQUAL_STRING("LongTag", logTag.c_str());
}

void test_StartPinnedTask() {
    // Test pinning to core 0
    bool result = testTaskManager->startTaskPinned(
        SimpleTestTask,
        "PinnedTest",
        4096,
        nullptr,
        1,
        0,  // Core 0
        "PIN"
    );
    
    TEST_ASSERT_TRUE(result);
    
    // Wait for task to start
    xEventGroupWaitBits(testEventGroup, TEST_TASK_STARTED, pdTRUE, pdTRUE, pdMS_TO_TICKS(1000));
    
    // Verify task info
    const auto& taskList = testTaskManager->getTaskList();
    TEST_ASSERT_EQUAL(1, taskList.size());
    TEST_ASSERT_EQUAL(0, taskList[0].coreID);
    TEST_ASSERT_TRUE(taskList[0].isPinned);
}

void test_GetTaskHandle() {
    TaskHandle_t handle = nullptr;
    
    bool result = testTaskManager->startTask(
        SimpleTestTask,
        "HandleTest",
        4096,
        nullptr,
        1,
        "HT",
        &handle
    );
    
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_NOT_NULL(handle);
    
    // Verify we can get the same handle by name
    TaskHandle_t retrievedHandle = testTaskManager->getTaskHandleByName("HandleTest");
    TEST_ASSERT_EQUAL(handle, retrievedHandle);
}

void test_TaskStateTracking() {
    TaskHandle_t handle = nullptr;
    
    bool result = testTaskManager->startTask(
        StateMonitorTestTask,
        "StateTest",
        4096,
        nullptr,
        1,
        "ST",
        &handle
    );
    
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_NOT_NULL(handle);
    testState.handle = handle;
    
    // Wait for task to start
    xEventGroupWaitBits(testEventGroup, TEST_TASK_STARTED, pdTRUE, pdTRUE, pdMS_TO_TICKS(1000));
    
    // Give time for state tracking
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // Check task state
    const auto& taskList = testTaskManager->getTaskList();
    TEST_ASSERT_EQUAL(1, taskList.size());
    
    // Task should have gone through states
    TEST_ASSERT_NOT_EQUAL(eInvalid, taskList[0].lastKnownState);
}

void test_WatchdogInitialization() {
    bool result = testTaskManager->initWatchdog(30, true);
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_TRUE(testTaskManager->isWatchdogInitialized());
}

void test_WatchdogTaskRegistration() {
    // Initialize watchdog first
    TEST_ASSERT_TRUE(testTaskManager->initWatchdog(30, true));
    
    // Start task with watchdog
    TaskManager::WatchdogConfig wdConfig = TaskManager::WatchdogConfig::enabled(false, 2000);
    
    bool result = testTaskManager->startTask(
        WatchdogTestTask,
        "WatchdogTask",
        4096,
        nullptr,
        1,
        "WDT",
        nullptr,
        wdConfig
    );
    
    TEST_ASSERT_TRUE(result);
    
    // Wait for task completion
    EventBits_t bits = xEventGroupWaitBits(
        testEventGroup,
        TEST_TASK_COMPLETED,
        pdTRUE,
        pdTRUE,
        pdMS_TO_TICKS(3000)
    );
    
    TEST_ASSERT_EQUAL(TEST_TASK_COMPLETED, bits & TEST_TASK_COMPLETED);
    TEST_ASSERT_TRUE(testState.watchdogFed);
}

void test_TaskStatistics() {
    // Create multiple tasks
    for (int i = 0; i < 3; i++) {
        char name[32];
        snprintf(name, sizeof(name), "StatTest%d", i);
        testTaskManager->startTask(SimpleTestTask, name, 4096, nullptr, 1);
    }
    
    // Wait for tasks to start
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // Get statistics
    TaskManager::TaskStatistics stats = testTaskManager->getTaskStatistics();
    
    TEST_ASSERT_GREATER_OR_EQUAL(3, stats.totalTasks);
    TEST_ASSERT_GREATER_OR_EQUAL(0, stats.runningTasks);
    TEST_ASSERT_GREATER_OR_EQUAL(0, stats.blockedTasks);
}

void test_TaskDeletion() {
    TaskHandle_t handle = nullptr;
    
    bool result = testTaskManager->startTask(
        SimpleTestTask,
        "DeleteTest",
        4096,
        nullptr,
        1,
        "DT",
        &handle
    );
    
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_NOT_NULL(handle);
    
    // Wait for task to complete and self-delete
    xEventGroupWaitBits(testEventGroup, TEST_TASK_COMPLETED, pdTRUE, pdTRUE, pdMS_TO_TICKS(1000));
    
    // Give time for cleanup
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // Task should no longer be valid
    TEST_ASSERT_FALSE(testTaskManager->isTaskValid(handle));
}

void test_InvalidOperations() {
    // Test null function pointer
    bool result = testTaskManager->startTask(
        nullptr,
        "NullTask",
        4096,
        nullptr,
        1
    );
    TEST_ASSERT_FALSE(result);
    
    // Test empty task name
    result = testTaskManager->startTask(
        SimpleTestTask,
        "",
        4096,
        nullptr,
        1
    );
    TEST_ASSERT_FALSE(result);
    
    // Test zero stack size
    result = testTaskManager->startTask(
        SimpleTestTask,
        "ZeroStack",
        0,
        nullptr,
        1
    );
    TEST_ASSERT_FALSE(result);
}

void test_LogTagManagement() {
    // Create tasks with different naming schemes
    TaskManager::TaskNames names1("TestTask1", "T1", "Test1");
    TaskManager::TaskNames names2("TestTask2", "T2", "Test2");
    
    testTaskManager->startTask(SimpleTestTask, names1, 4096, nullptr, 1);
    testTaskManager->startTask(SimpleTestTask, names2, 4096, nullptr, 1);
    
    // Get all log tags
    std::vector<std::string> tags = testTaskManager->getAllLogTags();
    TEST_ASSERT_GREATER_OR_EQUAL(2, tags.size());
    
    // Configure log levels
    testTaskManager->configureTaskLogLevel("TestTask1", ESP_LOG_DEBUG);
    testTaskManager->configureTaskGroupLogLevel("Test", ESP_LOG_INFO);
}

void test_MemoryLeaks() {
    size_t heapBefore = esp_get_free_heap_size();
    
    // Create and delete many tasks
    for (int i = 0; i < 10; i++) {
        char name[32];
        snprintf(name, sizeof(name), "LeakTest%d", i);
        
        bool result = testTaskManager->startTask(
            SimpleTestTask,
            name,
            4096,
            nullptr,
            1
        );
        TEST_ASSERT_TRUE(result);
    }
    
    // Wait for all tasks to complete
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Clean up deleted tasks
    testTaskManager->cleanupDeletedTasks();
    
    size_t heapAfter = esp_get_free_heap_size();
    
    // Allow for some overhead, but heap should be mostly recovered
    int32_t heapDiff = heapBefore - heapAfter;
    TEST_ASSERT_LESS_THAN(10000, heapDiff); // Max 10KB difference
}

// Forward declaration for stack size tests
void runStackSizeTests();

// ===== Test Runner =====

void runAllTests() {
    UNITY_BEGIN();
    
    // Basic functionality tests
    RUN_TEST(test_TaskManager_Construction);
    RUN_TEST(test_StartSimpleTask);
    RUN_TEST(test_StartTaskWithNames);
    RUN_TEST(test_StartPinnedTask);
    RUN_TEST(test_GetTaskHandle);
    
    // State and monitoring tests
    RUN_TEST(test_TaskStateTracking);
    RUN_TEST(test_TaskStatistics);
    RUN_TEST(test_TaskDeletion);
    
    // Watchdog tests
    RUN_TEST(test_WatchdogInitialization);
    RUN_TEST(test_WatchdogTaskRegistration);
    
    // Error handling tests
    RUN_TEST(test_InvalidOperations);
    
    // Advanced features tests
    RUN_TEST(test_LogTagManagement);
    RUN_TEST(test_MemoryLeaks);
    
    UNITY_END();
    
    // Run stack size tests separately
    Serial.println("\n--- Running Stack Size Fix Tests ---");
    runStackSizeTests();
}

