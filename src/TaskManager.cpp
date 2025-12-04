// TaskManager.cpp - Optimized implementation with smaller footprint (default)
#include "TaskManager.h"
#include <algorithm>

// Include exception header only when exceptions are enabled
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
#include <exception>
#endif

#ifdef ESP32
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#endif

// External logger instance removed - using LogInterface instead

const char* TaskManager::tag = "TM";  // Shortened tag

// Constructor with watchdog dependency injection
TaskManager::TaskManager(IWatchdog* watchdog)
    :
#if TM_ENABLE_DEBUG_TASK
    debugTaskHandle(nullptr),
#endif
    taskListMutex(nullptr)
    , isIterating_(false)
    , operationCount_(0)
    , lastHealthCheck_(0)
    , recoveryEnabled_(false)
    , leakDetectionEnabled_(false)
#if TM_ENABLE_WATCHDOG
    , watchdog_(watchdog ? watchdog : &nullWatchdog_)
    , watchdogInitialized(false)
    , watchdogTimeoutSeconds(30)
#endif
#if TM_ENABLE_DEBUG_TASK
    , resourceLogPeriod(pdMS_TO_TICKS(20000))
#endif
{
    initMutex();

    // Reserve initial capacity to avoid reallocation
    taskList_.reserve(10);

    // Initialize recovery system
    TaskRecovery::initialize();
}

// Destructor
TaskManager::~TaskManager() {
    if (taskListMutex != nullptr) {
        // Use timeout to prevent hanging on destruction
        RecursiveMutexGuard guard(taskListMutex, pdMS_TO_TICKS(5000));
        if (guard.hasLock()) {
            // Mark as iterating to prevent concurrent modifications
            isIterating_ = true;
            
            // Clean up all tasks safely
            for (auto& task : taskList_) {
                if (task.taskHandle != nullptr) {
                    // Check if task is still valid before operations
                    eTaskState state = eTaskGetState(task.taskHandle);
                    if (state != eDeleted && state != eInvalid) {
                        // Report any resource leaks
                        if (leakDetectionEnabled_) {
                            ResourceLeakDetector::reportTaskLeaks(task.taskHandle);
                        }
                        
                        #if TM_ENABLE_WATCHDOG
                        if (task.watchdogEnabled && watchdogInitialized) {
                            esp_task_wdt_delete(task.taskHandle);
                        }
                        #endif
                        vTaskDelete(task.taskHandle);
                        // Small delay to allow task cleanup
                        vTaskDelay(pdMS_TO_TICKS(10));
                    }
                }
            }
            taskList_.clear();
            isIterating_ = false;
        }
        vSemaphoreDelete(taskListMutex);
    }
}

void TaskManager::initMutex() {
    // Use recursive mutex to prevent deadlocks
    taskListMutex = xSemaphoreCreateRecursiveMutex();
    if (taskListMutex == nullptr) {
        TASKM_LOG_E( "Mutex create failed");
    }
}

// Core task creation method
bool TaskManager::startTask(void (*taskFunction)(void*), 
                           const char* taskName,
                           uint16_t stackSize, 
                           void* parameter, 
                           UBaseType_t priority,
                           const WatchdogConfig& watchdogConfig) {
    // Input validation
    if (!taskFunction || !taskName || stackSize < configMINIMAL_STACK_SIZE) {
        return false;
    }
    
    // Validate operation safety
    if (!validateTaskOperation()) {
        return false;
    }
    
    // Create task parameters
    TaskParams* params = new TaskParams{taskFunction, parameter};
    
    // Create task with safe wrapper
    TaskHandle_t taskHandle = nullptr;
    BaseType_t result = xTaskCreate(
        safeTaskWrapper,  // Use safe wrapper
        taskName,  // FreeRTOS will truncate if needed
        stackSize / sizeof(StackType_t),  // Convert bytes to words
        static_cast<void*>(params),
        priority,
        &taskHandle
    );
    
    if (result != pdPASS) {
        delete params;
        return false;
    }
    
    // Create minimal TaskInfo
    TaskInfo taskInfo = {};
    taskInfo.taskHandle = taskHandle;
    strncpy(taskInfo.taskName, taskName, TM_MAX_TASK_NAME_LEN - 1);
    taskInfo.taskName[TM_MAX_TASK_NAME_LEN - 1] = '\0';
    
    #if TM_ENABLE_TASK_NAMES
    // Generate simple abbreviation
    for (int i = 0; i < 3 && taskName[i]; i++) {
        taskInfo.abbreviatedName[i] = taskName[i];
    }
    taskInfo.abbreviatedName[3] = '\0';
    #endif
    
    taskInfo.startTime = xTaskGetTickCount();
    taskInfo.totalStackSize = stackSize;
    taskInfo.isPinned = 0;
    taskInfo.coreID = 0;
    taskInfo.hasStackWarning = 0;
    taskInfo.hasCrashed = 0;
    taskInfo.stackHighWaterMark.store(stackSize);
    
    #if TM_ENABLE_WATCHDOG
    taskInfo.watchdogEnabled = 0;
    taskInfo.watchdogCritical = 0;
    taskInfo.watchdogFeedInterval = 0;
    taskInfo.watchdogMissedFeeds.store(0);
    taskInfo.lastWatchdogFeed.store(0);
    #endif
    
    // Add to list
    addTaskToList(taskInfo);
    
    // Register with recovery system if enabled
    if (recoveryEnabled_) {
        TaskRecovery::TaskRecoveryInfo recoveryInfo;
        recoveryInfo.handle = taskHandle;
        strncpy(recoveryInfo.taskName, taskName, configMAX_TASK_NAME_LEN - 1);
        recoveryInfo.taskFunction = taskFunction;
        recoveryInfo.taskParameter = parameter;
        recoveryInfo.stackSize = stackSize;
        recoveryInfo.priority = priority;
        recoveryInfo.isPinned = false;
        TaskRecovery::registerTask(recoveryInfo);
    }
    
    #if TM_ENABLE_WATCHDOG
    // Configure watchdog if requested
    if (watchdogConfig.enableWatchdog && watchdogInitialized) {
        configureTaskWatchdog(taskName, watchdogConfig);
    }
    #endif
    
    return true;
}

// Pinned task creation
bool TaskManager::startTaskPinned(void (*taskFunction)(void*), 
                                 const char* taskName,
                                 uint16_t stackSize, 
                                 void* parameter, 
                                 UBaseType_t priority,
                                 BaseType_t coreID,
                                 const WatchdogConfig& watchdogConfig) {
    // Input validation
    if (!taskFunction || !taskName || stackSize < configMINIMAL_STACK_SIZE) {
        return false;
    }
    
    TaskParams* params = new TaskParams{taskFunction, parameter};
    TaskHandle_t taskHandle = nullptr;
    
    BaseType_t result = xTaskCreatePinnedToCore(
        safeTaskWrapper,  // Use safe wrapper
        taskName,
        stackSize / sizeof(StackType_t),  // Convert bytes to words
        static_cast<void*>(params),
        priority,
        &taskHandle,
        coreID
    );
    
    if (result != pdPASS) {
        delete params;
        return false;
    }
    
    // Create TaskInfo
    TaskInfo taskInfo = {};
    taskInfo.taskHandle = taskHandle;
    strncpy(taskInfo.taskName, taskName, TM_MAX_TASK_NAME_LEN - 1);
    taskInfo.taskName[TM_MAX_TASK_NAME_LEN - 1] = '\0';
    
    #if TM_ENABLE_TASK_NAMES
    for (int i = 0; i < 3 && taskName[i]; i++) {
        taskInfo.abbreviatedName[i] = taskName[i];
    }
    taskInfo.abbreviatedName[3] = '\0';
    #endif
    
    taskInfo.startTime = xTaskGetTickCount();
    taskInfo.totalStackSize = stackSize;
    taskInfo.isPinned = 1;
    taskInfo.coreID = coreID;
    taskInfo.hasStackWarning = 0;
    taskInfo.hasCrashed = 0;
    taskInfo.stackHighWaterMark.store(stackSize);
    
    addTaskToList(taskInfo);
    
    // Register with recovery system if enabled
    if (recoveryEnabled_) {
        TaskRecovery::TaskRecoveryInfo recoveryInfo;
        recoveryInfo.handle = taskHandle;
        strncpy(recoveryInfo.taskName, taskName, configMAX_TASK_NAME_LEN - 1);
        recoveryInfo.taskFunction = taskFunction;
        recoveryInfo.taskParameter = parameter;
        recoveryInfo.stackSize = stackSize;
        recoveryInfo.priority = priority;
        recoveryInfo.coreID = coreID;
        recoveryInfo.isPinned = true;
        TaskRecovery::registerTask(recoveryInfo);
    }
    
    #if TM_ENABLE_WATCHDOG
    if (watchdogConfig.enableWatchdog && watchdogInitialized) {
        configureTaskWatchdog(taskName, watchdogConfig);
    }
    #endif
    
    return true;
}

// Task wrapper
void TaskManager::generalTaskWrapper(void* pvParameters) {
    TaskParams* taskParams = static_cast<TaskParams*>(pvParameters);
    
    if (taskParams && taskParams->function) {
        taskParams->function(taskParams->arg);
    }
    
    delete taskParams;
    vTaskDelete(nullptr);
}

// Safe task wrapper with crash detection and optional exception handling
void TaskManager::safeTaskWrapper(void* pvParameters) {
    TaskParams* taskParams = static_cast<TaskParams*>(pvParameters);

    if (taskParams && taskParams->function) {
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
        // Exception handling enabled - wrap task execution in try/catch
        try {
            taskParams->function(taskParams->arg);
        } catch (const std::exception& e) {
            const char* taskName = pcTaskGetName(nullptr);
            TASKM_LOG_E("Task %s threw std::exception: %s",
                        taskName ? taskName : "unknown", e.what());
            // Attempt recovery through TaskRecovery mechanism
            TaskRecovery::attemptRecovery(taskName ? taskName : "unknown",
                                          TaskRecovery::FailureType::EXCEPTION);
        } catch (...) {
            const char* taskName = pcTaskGetName(nullptr);
            TASKM_LOG_E("Task %s threw unknown exception",
                        taskName ? taskName : "unknown");
            TaskRecovery::attemptRecovery(taskName ? taskName : "unknown",
                                          TaskRecovery::FailureType::EXCEPTION);
        }
#else
        // Exceptions disabled - direct execution (most embedded builds)
        taskParams->function(taskParams->arg);
#endif
    }

    delete taskParams;
    vTaskDelete(nullptr);
}

// Add task to list
void TaskManager::addTaskToList(const TaskInfo& taskInfo) {
    // Wait for any iteration to complete
    while (isIterating_.load()) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    
    RecursiveMutexGuard guard(taskListMutex, pdMS_TO_TICKS(5000));
    if (guard.hasLock()) {
        operationCount_.fetch_add(1);
        taskList_.push_back(taskInfo);
        operationCount_.fetch_sub(1);
        MemoryBarrier::write(); // Ensure visibility across cores
    } else {
        TASKM_LOG_E( "Failed to add task - mutex timeout");
    }
}

// Get task statistics
TaskManager::TaskStatistics TaskManager::getTaskStatistics() const {
    TaskStatistics stats = {};
    
    // Use a safe copy to avoid holding mutex during iteration
    auto taskListCopy = copyTaskListSafely();
    
    stats.totalTasks = taskListCopy.size();
    
    for (const auto& task : taskListCopy) {
        if (task.isPinned) {
            stats.pinnedTasks++;
        }
        #if TM_ENABLE_WATCHDOG
        if (task.watchdogEnabled) {
            stats.watchdogTasks++;
        }
        #endif
    }
    
    return stats;
}

// Clean up deleted tasks
void TaskManager::cleanupDeletedTasks() {
    // Prevent concurrent modifications
    if (isIterating_.exchange(true)) {
        return; // Already iterating, skip cleanup
    }
    
    RecursiveMutexGuard guard(taskListMutex, pdMS_TO_TICKS(1000));
    if (guard.hasLock()) {
        auto it = taskList_.begin();
        while (it != taskList_.end()) {
            if (it->taskHandle != nullptr) {
                eTaskState state = eTaskGetState(it->taskHandle);
                if (state == eDeleted || state == eInvalid) {
                    #if TM_ENABLE_WATCHDOG
                    // Clean up watchdog if enabled
                    if (it->watchdogEnabled && watchdogInitialized) {
                        esp_task_wdt_delete(it->taskHandle);
                    }
                    #endif
                    it = taskList_.erase(it);
                    continue;
                }
                
                // Check stack health
                if (!it->hasStackWarning) {
                    auto stackStatus = StackMonitor::checkTaskStack(it->taskHandle);
                    if (stackStatus.warningLevel) {
                        it->hasStackWarning = 1;
                        TASKM_LOG_W( "Task %s low stack: %lu bytes", 
                                  it->taskName, (unsigned long)stackStatus.freeBytes);
                    }
                    it->stackHighWaterMark.store(stackStatus.highWaterMark);
                }
            }
            ++it;
        }
        
        // Perform health check periodically
        TickType_t now = xTaskGetTickCount();
        if ((now - lastHealthCheck_) > pdMS_TO_TICKS(5000)) {
            monitorTaskStacks();
            lastHealthCheck_ = now;
        }
    }
    
    isIterating_ = false;
    MemoryBarrier::full(); // Ensure all changes are visible
}

// Task state to string
const char* TaskManager::taskStateToStringShort(eTaskState taskState) {
    switch (taskState) {
        case eRunning:   return "RUN";
        case eReady:     return "RDY";
        case eBlocked:   return "BLK";
        case eSuspended: return "SUS";
        case eDeleted:   return "DEL";
        default:         return "UNK";
    }
}

#if TM_ENABLE_WATCHDOG
// Watchdog implementation using injected IWatchdog interface
bool TaskManager::initWatchdog(uint32_t timeoutSeconds, bool panicOnTimeout) {
    // Use the injected watchdog instance
    if (watchdog_->init(timeoutSeconds, panicOnTimeout)) {
        watchdogInitialized = true;
        watchdogTimeoutSeconds = timeoutSeconds;
        TASKM_LOG_I("Watchdog initialized via injected interface");
        return true;
    }

    TASKM_LOG_E("Failed to initialize watchdog");
    return false;
}

bool TaskManager::feedWatchdog() {
    // Delegate to the injected watchdog interface
    bool result = watchdog_->feed();
    
    if (result) {
        // Update internal stats for compatibility
        TaskHandle_t currentTask = xTaskGetCurrentTaskHandle();
        if (currentTask) {
            TickType_t now = xTaskGetTickCount();
            RecursiveMutexGuard guard(taskListMutex, pdMS_TO_TICKS(100));
            if (guard.hasLock()) {
                for (auto& task : taskList_) {
                    if (task.taskHandle == currentTask && task.watchdogEnabled) {
                        task.lastWatchdogFeed.store(now);
                        task.watchdogMissedFeeds.store(0);
                        break;
                    }
                }
            }
        }
    }
    
    return result;
}

bool TaskManager::registerCurrentTaskWithWatchdog(const char* taskName, const WatchdogConfig& config) {
    if (!config.enableWatchdog) {
        return true;  // Nothing to do
    }
    
    // Register with external watchdog library
    uint32_t feedInterval = config.feedIntervalMs > 0 ? 
        config.feedIntervalMs : (watchdogTimeoutSeconds * 1000 / 5);
    
    // Check if already registered with ESP-IDF watchdog
    TaskHandle_t currentHandle = xTaskGetCurrentTaskHandle();
    esp_err_t status = esp_task_wdt_status(currentHandle);
    
    if (status == ESP_OK) {
        // Already registered, just update our tracking
        TASKM_LOG_I("Task %s already registered with ESP-IDF watchdog", taskName);
    } else {
        // Not registered, try to register
        if (!watchdog_->registerCurrentTask(taskName, config.criticalTask, feedInterval)) {
            TASKM_LOG_E("Failed to register task %s with watchdog", taskName);
            return false;
        }
    }
    
    // Update internal tracking
    TaskHandle_t handle = xTaskGetCurrentTaskHandle();
    RecursiveMutexGuard guard(taskListMutex, portMAX_DELAY);
    if (guard.hasLock()) {
        for (auto& task : taskList_) {
            if (task.taskHandle == handle) {
                task.watchdogEnabled = 1;
                task.watchdogCritical = config.criticalTask ? 1 : 0;
                task.watchdogFeedInterval = feedInterval;
                task.lastWatchdogFeed.store(xTaskGetTickCount());
                task.watchdogMissedFeeds.store(0);
                return true;
            }
        }
        
        // Task not in our list but successfully registered with watchdog
        // This happens for tasks created outside TaskManager (like Arduino's loopTask)
        TASKM_LOG_I("Task %s registered with watchdog but not tracked by TaskManager", taskName);
        return true;  // Still return success since watchdog registration succeeded
    }
    
    return false;
}

bool TaskManager::unregisterCurrentTaskFromWatchdog() {
    return watchdog_->unregisterCurrentTask();
}

bool TaskManager::configureTaskWatchdog(const char* taskName, const WatchdogConfig& config) {
    if (!watchdogInitialized && config.enableWatchdog) {
        if (!initWatchdog(30, true)) {
            return false;
        }
    }
    
    if (xSemaphoreTake(taskListMutex, portMAX_DELAY) == pdTRUE) {
        bool result = false;
        for (auto& task : taskList_) {
            if (strncmp(task.taskName, taskName, TM_MAX_TASK_NAME_LEN) == 0) {
                result = configureTaskWatchdogInternal(task, config);
                break;
            }
        }
        xSemaphoreGive(taskListMutex);
        return result;
    }
    
    return false;
}

bool TaskManager::configureTaskWatchdogInternal(TaskInfo& task, const WatchdogConfig& config) {
    if (!task.taskHandle) return false;
    
    // Note: External Watchdog library requires tasks to register themselves
    // from their own context. This method only updates internal tracking.
    // The actual registration should happen when the task calls registerCurrentTaskWithWatchdog()
    
    if (config.enableWatchdog) {
        task.watchdogEnabled = 1;
        task.watchdogCritical = config.criticalTask ? 1 : 0;
        task.watchdogFeedInterval = config.feedIntervalMs > 0 ? 
            config.feedIntervalMs : (watchdogTimeoutSeconds * 1000 / 5);
        task.watchdogMissedFeeds.store(0);
        task.lastWatchdogFeed.store(xTaskGetTickCount());
        
        TASKM_LOG_I( "Task %s configured for watchdog (must register from task context)", 
                   task.taskName);
        return true;
    } else {
        task.watchdogEnabled = 0;
        return true;
    }
}

void TaskManager::checkWatchdogHealth() {
    if (!watchdogInitialized) return;

    // Use injected watchdog interface for health check
    size_t unhealthyCount = watchdog_->checkHealth();
    if (unhealthyCount > 0) {
        TASKM_LOG_W( "%zu tasks are not feeding watchdog properly", unhealthyCount);
    }
    
    // Also maintain internal tracking for compatibility
    TickType_t now = xTaskGetTickCount();
    
    for (auto& task : taskList_) {
        if (task.watchdogEnabled && task.taskHandle != nullptr) {
            TickType_t lastFeed = task.lastWatchdogFeed.load();
            TickType_t timeSinceFeed = now - lastFeed;
            
            if (timeSinceFeed > pdMS_TO_TICKS(task.watchdogFeedInterval)) {
                uint16_t missedFeeds = task.watchdogMissedFeeds.fetch_add(1) + 1;
                
                if (missedFeeds > 3) {
                    TASKM_LOG_E( "Task %s missed %lu watchdog feeds", 
                              task.taskName, (unsigned long)missedFeeds);
                    
                    if (task.watchdogCritical) {
                        handleTaskFailure(task);
                    }
                }
            }
        }
    }
}

void TaskManager::logWatchdogStats() const {
    if (!watchdogInitialized) return;
    
    TASKM_LOG_I( "=== WDT Stats ===");
    
    if (xSemaphoreTake(taskListMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        uint16_t wdtTasks = 0;
        for (const auto& task : taskList_) {
            if (task.watchdogEnabled && task.taskHandle != nullptr) {
                eTaskState state = eTaskGetState(task.taskHandle);
                if (state != eInvalid && state != eDeleted) {
                    wdtTasks++;
                    TASKM_LOG_I( "  %s: %s, Int=%lu", 
                              task.taskName,
                              task.watchdogCritical ? "CRIT" : "NORM", 
                              (unsigned long)task.watchdogFeedInterval);
                }
            }
        }
        TASKM_LOG_I( "Total: %lu tasks", (unsigned long)wdtTasks);
        xSemaphoreGive(taskListMutex);
    }
}
#endif // TM_ENABLE_WATCHDOG

// Safety helper methods
bool TaskManager::acquireMutexSafely(TickType_t timeout) {
    return xSemaphoreTakeRecursive(taskListMutex, timeout) == pdTRUE;
}

void TaskManager::releaseMutexSafely() {
    xSemaphoreGiveRecursive(taskListMutex);
}

std::vector<TaskManager::TaskInfo> TaskManager::copyTaskListSafely() const {
    std::vector<TaskInfo> copy;
    
    // Cast away const for mutex operation (safe because we're only reading)
    TaskManager* mutableThis = const_cast<TaskManager*>(this);
    RecursiveMutexGuard guard(mutableThis->taskListMutex, pdMS_TO_TICKS(1000));
    
    if (guard.hasLock()) {
        copy = taskList_;
    }
    
    return copy;
}

void TaskManager::monitorTaskStacks() {
    for (auto& task : taskList_) {
        if (task.taskHandle != nullptr) {
            auto status = StackMonitor::checkTaskStack(task.taskHandle);
            task.stackHighWaterMark.store(status.highWaterMark);
            
            if (status.criticalLevel && !task.hasStackWarning) {
                task.hasStackWarning = 1;
                TASKM_LOG_E( "CRITICAL: Task %s stack only %lu bytes free!", 
                          task.taskName, (unsigned long)status.freeBytes);
                
                // Consider suspending task to prevent crash
                if (status.freeBytes < 128) {
                    vTaskSuspend(task.taskHandle);
                    task.hasCrashed = 1;
                    TASKM_LOG_E( "Task %s suspended due to critical stack", 
                              task.taskName);
                }
            }
        }
    }
}

bool TaskManager::checkTaskStackHealth(const TaskInfo& task) const {
    if (task.taskHandle == nullptr) return false;
    
    auto status = StackMonitor::checkTaskStack(task.taskHandle);
    return !status.criticalLevel;
}

bool TaskManager::isTaskHealthy(TaskHandle_t handle) const {
    if (handle == nullptr) return false;
    
    eTaskState state = eTaskGetState(handle);
    if (state == eDeleted || state == eInvalid) return false;
    
    auto status = StackMonitor::checkTaskStack(handle);
    return !status.criticalLevel;
}

void TaskManager::handleTaskFailure(TaskInfo& task) {
    TASKM_LOG_E( "Handling failure for task: %s", task.taskName);
    
    // Determine failure type
    TaskRecovery::FailureType failureType = TaskRecovery::FailureType::UNKNOWN;
    
    if (task.hasStackWarning) {
        failureType = TaskRecovery::FailureType::STACK_OVERFLOW;
    } else if (task.watchdogEnabled && task.watchdogMissedFeeds.load() > 3) {
        failureType = TaskRecovery::FailureType::WATCHDOG_TIMEOUT;
    }
    
    // Attempt recovery if enabled
    bool recovered = false;
    if (recoveryEnabled_) {
        recovered = TaskRecovery::attemptRecovery(task.taskName, failureType);
    }
    
    if (!recovered) {
        #if TM_ENABLE_WATCHDOG
        if (task.watchdogEnabled && watchdogInitialized) {
            esp_task_wdt_delete(task.taskHandle);
            task.watchdogEnabled = 0;
        }
        #endif
        
        task.hasCrashed = 1;
        
        // Try to suspend instead of delete to preserve debugging info
        if (task.taskHandle != nullptr) {
            vTaskSuspend(task.taskHandle);
        }
    }
}

bool TaskManager::validateTaskOperation() const {
    // Check if we're in a safe state for operations
    if (isIterating_.load()) {
        TASKM_LOG_W( "Operation blocked - iteration in progress");
        return false;
    }
    
    if (operationCount_.load() > 10) {
        TASKM_LOG_W( "High concurrent operations: %lu", (unsigned long)operationCount_.load());
    }
    
    return true;
}

#if TM_ENABLE_DEBUG_TASK
void TaskManager::setResourceLogPeriod(uint32_t interval_ms) {
    resourceLogPeriod = pdMS_TO_TICKS(interval_ms);
}

void TaskManager::debugTask() {
    TASKM_LOG_I( "Debug task started");
    
    TickType_t lastLogTime = xTaskGetTickCount();
    TickType_t lastHealthCheck = xTaskGetTickCount();
    int cleanupCounter = 0;
    
    while (true) {
        // Periodic resource logging
        if ((xTaskGetTickCount() - lastLogTime) >= resourceLogPeriod) {
            printResourceUsage();
            lastLogTime = xTaskGetTickCount();
        }
        
        // Periodic health monitoring (every 2 seconds)
        if ((xTaskGetTickCount() - lastHealthCheck) >= pdMS_TO_TICKS(2000)) {
            monitorTaskStacks();
            #if TM_ENABLE_WATCHDOG
            checkWatchdogHealth();
            #endif
            lastHealthCheck = xTaskGetTickCount();
        }
        
        // Periodic cleanup
        if (++cleanupCounter >= 10) {
            cleanupCounter = 0;
            cleanupDeletedTasks();
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void TaskManager::debugTaskWrapper(void* pvParameters) {
    if (pvParameters) {
        TaskManager* instance = static_cast<TaskManager*>(pvParameters);
        instance->debugTask();
    }
    vTaskDelete(nullptr);
}

void TaskManager::printResourceUsage() {
    // Log heap usage
    size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    size_t minHeapEver = xPortGetMinimumEverFreeHeapSize();
    
    TASKM_LOG_I( "Heap: Free=%lu, MinEver=%lu", (unsigned long)freeHeap, (unsigned long)minHeapEver);
    
    // Use safe copy to avoid holding mutex during logging
    auto taskListCopy = copyTaskListSafely();
    
    for (const auto& task : taskListCopy) {
        if (task.taskHandle != nullptr) {
            eTaskState state = eTaskGetState(task.taskHandle);
            if (state != eInvalid && state != eDeleted) {
                uint32_t stackHWM = task.stackHighWaterMark.load();
                const char* healthStatus = task.hasStackWarning ? " [WARN]" : "";
                const char* crashStatus = task.hasCrashed ? " [CRASH]" : "";
                
                #if TM_ENABLE_TASK_NAMES
                TASKM_LOG_I( "%s: %s, Stack=%lu%s%s", 
                          task.abbreviatedName,
                          taskStateToStringShort(state),
                          (unsigned long)stackHWM,
                          healthStatus,
                          crashStatus);
                #else
                TASKM_LOG_I( "%s: %s, Stack=%lu%s%s", 
                          task.taskName,
                          taskStateToStringShort(state),
                          (unsigned long)stackHWM,
                          healthStatus,
                          crashStatus);
                #endif
            }
        }
    }
}
#endif // TM_ENABLE_DEBUG_TASK

// Recovery configuration methods
void TaskManager::enableTaskRecovery(bool enable) {
    recoveryEnabled_ = enable;
    
    if (enable) {
        TASKM_LOG_I( "Task recovery enabled");
    }
}

void TaskManager::setRecoveryPolicy(TaskRecovery::FailureType failure,
                                   TaskRecovery::RecoveryAction action,
                                   uint32_t maxRetries) {
    TaskRecovery::RecoveryPolicy policy;
    policy.failureType = failure;
    policy.action = action;
    policy.maxRetries = maxRetries;
    policy.backoffMs = 1000; // Default 1 second
    
    TaskRecovery::addPolicy(policy);
}

// Resource leak detection methods
void TaskManager::enableLeakDetection(bool enable) {
    leakDetectionEnabled_ = enable;
    
    if (enable) {
        ResourceLeakDetector::initialize();
        ResourceLeakDetector::setTaskManager(this);  // Register ourselves for name lookup
        ResourceLeakDetector::enableTracking(true);
        TASKM_LOG_I( "Resource leak detection enabled");
    } else {
        ResourceLeakDetector::enableTracking(false);
        TASKM_LOG_I( "Resource leak detection disabled");
    }
}

void TaskManager::checkResourceLeaks() {
    if (!leakDetectionEnabled_) return;
    
    // Perform health check
    ResourceLeakDetector::performHealthCheck();
    
    // Check for leaks in deleted tasks
    auto taskListCopy = copyTaskListSafely();
    for (const auto& task : taskListCopy) {
        eTaskState state = eTaskGetState(task.taskHandle);
        if (state == eDeleted || state == eInvalid) {
            ResourceLeakDetector::reportTaskLeaks(task.taskHandle);
        }
    }
}

std::vector<ResourceLeakDetector::TaskResourceStats> TaskManager::getResourceStats() const {
    if (!leakDetectionEnabled_) {
        return std::vector<ResourceLeakDetector::TaskResourceStats>();
    }
    return ResourceLeakDetector::getAllTaskStats();
}

bool TaskManager::stopTask(TaskHandle_t handle) {
    if (!handle) {
        TASKM_LOG_E( "Invalid task handle");
        return false;
    }
    
    RecursiveMutexGuard guard(taskListMutex, pdMS_TO_TICKS(5000));
    if (!guard.hasLock()) {
        TASKM_LOG_E( "Failed to acquire mutex for stopTask");
        return false;
    }
    
    // Find and remove task
    auto it = std::find_if(taskList_.begin(), taskList_.end(),
        [handle](const TaskInfo& task) { return task.taskHandle == handle; });
    
    if (it != taskList_.end()) {
        // Unregister from watchdog if enabled
        #if TM_ENABLE_WATCHDOG
        if (it->watchdogEnabled && watchdogInitialized) {
            watchdog_->unregisterTaskByHandle(handle, it->taskName);
        }
        #endif
        
        // Store name for logging
        char taskName[TM_MAX_TASK_NAME_LEN];
        strncpy(taskName, it->taskName, TM_MAX_TASK_NAME_LEN - 1);
        taskName[TM_MAX_TASK_NAME_LEN - 1] = '\0';
        
        // Remove from list first
        taskList_.erase(it);
        
        // Delete the FreeRTOS task
        vTaskDelete(handle);
        
        TASKM_LOG_I( "Task %s stopped successfully", taskName);
        return true;
    }
    
    TASKM_LOG_W( "Task handle not found in task list");
    return false;
}