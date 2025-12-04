/**
 * @file TaskManager.cpp
 * @brief Implementation of the TaskManager class.
 */

#include "TaskManager_Full.h"

#include <algorithm>
#include <memory>
#include <sstream>
#include <unordered_map>
#ifdef ESP32
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <Arduino.h>  // For psramFound() and ESP object
#endif
// External logger instance removed - using LogInterface instead

const char* TaskManager::tag = "TaskManager";

// make_unique implementation for C++11
#if __cplusplus <= 201103L
namespace std {
template <typename T, typename... Args>
std::unique_ptr<T> make_unique(Args&&... args) {
    return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}
}  // namespace std
#endif

// RAII Wrapper Class for MemberFunctionWrapperData
class MemberFunctionWrapper {
   public:
    MemberFunctionWrapper(TaskManager* instance, void (TaskManager::*memberFunction)(void))
        : data(new TaskManager::MemberFunctionWrapperData{instance, memberFunction}) {}

    ~MemberFunctionWrapper() { delete data; }

    // Disallow copy operations
    MemberFunctionWrapper(const MemberFunctionWrapper&) = delete;
    MemberFunctionWrapper& operator=(const MemberFunctionWrapper&) = delete;

    // Explicitly implement move operations for C++11
    MemberFunctionWrapper(MemberFunctionWrapper&& other) noexcept : data(other.data) {
        other.data = nullptr;
    }

    MemberFunctionWrapper& operator=(MemberFunctionWrapper&& other) noexcept {
        if (this != &other) {
            delete data;
            data = other.data;
            other.data = nullptr;
        }
        return *this;
    }

    TaskManager::MemberFunctionWrapperData* getData() const { return data; }

   private:
    TaskManager::MemberFunctionWrapperData* data;
};

/**
 * @brief Method to initialize the mutex.
 */
void TaskManager::initMutex() {
    taskListMutex = xSemaphoreCreateMutex();
    if (taskListMutex == nullptr) {
        // Handle error: Failed to create the mutex
        TASKM_LOG_E( "Failed to create task list mutex");
    }
}

/**
 * @brief Constructor for TaskManager.
 */
TaskManager::TaskManager() {
    // Initialize the mutex for task list
    initMutex();

    const int defaultQueueDepth = 100;  // Adjust this value as needed

    // Create the queue with a certain depth
    taskStateQueue = xQueueCreate(defaultQueueDepth, sizeof(TaskStateMessage));

    if (taskStateQueue == nullptr) {
        // Handle error: Failed to create the queue
        TASKM_LOG_E( "Failed to create task state queue");
    }

    // Other initialization if needed
}

/**
 * @brief Destructor for TaskManager.
 */
TaskManager::~TaskManager() {
    // Clean up tasks with mutex protection
    if (taskListMutex != nullptr) {
        {
            MutexGuard lock(taskListMutex);
            if (lock.hasLock()) {
                // Clean up all tasks
                for (std::vector<TaskInfo>::iterator it = taskList_.begin(); it != taskList_.end(); ++it) {
                    if (it->taskHandle != nullptr) {
                        // Remove from watchdog if enabled
                        if (it->watchdogEnabled && watchdogInitialized) {
                            esp_task_wdt_delete(it->taskHandle);
                        }
                        vTaskDelete(it->taskHandle);
                    }
                }
                taskList_.clear();
            }
        } // MutexGuard released here
        
        vSemaphoreDelete(taskListMutex);
        taskListMutex = nullptr;
    }

    if (taskStateQueue != nullptr) {
        vQueueDelete(taskStateQueue);
        taskStateQueue = nullptr;
    }
}

bool TaskManager::initWatchdog(uint32_t timeoutSeconds, bool panicOnTimeout) {
    TASKM_LOG_I( "Checking watchdog status");

    // Check if TWDT is already initialized
    esp_err_t err = esp_task_wdt_status(nullptr);

    if (err == ESP_OK) {
        // TWDT already initialized by Arduino framework - just use it
        TASKM_LOG_I(
                   "Watchdog already initialized by system, using existing configuration");
        watchdogInitialized = true;
        watchdogTimeoutSeconds = 5;  // Arduino typically uses 5 seconds

        // Success - watchdog is available
        return true;
    }

    // Try to initialize if not already done
    TASKM_LOG_I( "Initializing watchdog with %lu second timeout", (unsigned long)timeoutSeconds);
    watchdogTimeoutSeconds = timeoutSeconds;

#if ESP_IDF_VERSION_MAJOR >= 5
    esp_task_wdt_config_t wdtConfig = {
        .timeout_ms = timeoutSeconds * 1000, .idle_core_mask = 0, .trigger_panic = panicOnTimeout};
    err = esp_task_wdt_init(&wdtConfig);
#else
    err = esp_task_wdt_init(timeoutSeconds, panicOnTimeout);
#endif

    if (err != ESP_OK) {
        // Error 259 (0x103) is ESP_ERR_INVALID_STATE - already initialized
        if (err == 0x103 || err == ESP_ERR_INVALID_STATE) {
            TASKM_LOG_I( "Watchdog was already initialized, continuing");
            watchdogInitialized = true;
            return true;
        }

        TASKM_LOG_E( "Failed to initialize watchdog: error 0x%X (%d)", err, err);
        return false;
    }

    watchdogInitialized = true;
    TASKM_LOG_I( "Watchdog initialized successfully");
    return true;
}

bool TaskManager::feedWatchdog() {
    // First check if we're registered - use thread-local storage or check with ESP-IDF
    esp_err_t checkErr = esp_task_wdt_status(nullptr);
    
    if (checkErr == ESP_ERR_NOT_FOUND) {
        // Current task not in watchdog list, try to add it
        esp_err_t addErr = esp_task_wdt_add(nullptr);
        if (addErr == ESP_OK) {
            TASKM_LOG_I( "Current task added to watchdog");
        } else if (addErr == ESP_ERR_INVALID_ARG) {
            // This shouldn't happen but handle it
            TASKM_LOG_D( "Task already in watchdog (unexpected)");
        } else {
            TASKM_LOG_W( "Failed to add current task to watchdog: %d", addErr);
            return false;
        }
    }

    // Now try to feed the watchdog
    esp_err_t err = esp_task_wdt_reset();

    if (err == ESP_ERR_NOT_FOUND) {
        // May happen early during boot — treat as OK
        return true;
    }

    if (err != ESP_OK) {
        TASKM_LOG_E( "Failed to feed watchdog: %d", err);
        
        // Track missed feed for the current task
        TaskHandle_t currentTask = xTaskGetCurrentTaskHandle();
        if (currentTask && xSemaphoreTake(taskListMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            for (std::vector<TaskInfo>::iterator it = taskList_.begin(); 
                 it != taskList_.end(); ++it) {
                if (it->taskHandle == currentTask && it->watchdogEnabled) {
                    it->watchdogMissedFeeds++;
                    break;
                }
            }
            xSemaphoreGive(taskListMutex);
        }
        
        return false;
    }

    // Successful feed - update statistics for the current task
    TaskHandle_t currentTask = xTaskGetCurrentTaskHandle();
    if (currentTask && xSemaphoreTake(taskListMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        TickType_t currentTime = xTaskGetTickCount();
        
        for (std::vector<TaskInfo>::iterator it = taskList_.begin(); 
             it != taskList_.end(); ++it) {
            if (it->taskHandle == currentTask && it->watchdogEnabled) {
                // Update feed statistics
                it->totalWatchdogFeeds++;
                
                // Check if we're feeding too late (missed interval)
                if (it->lastWatchdogFeed != 0) {
                    TickType_t timeSinceLastFeed = currentTime - it->lastWatchdogFeed;
                    if (timeSinceLastFeed > pdMS_TO_TICKS(it->watchdogFeedInterval)) {
                        it->watchdogMissedFeeds++;
                        TASKM_LOG_W( 
                                   "Task %s fed watchdog late: %u ms (expected: %u ms)",
                                   it->taskName.c_str(), 
                                   pdTICKS_TO_MS(timeSinceLastFeed),
                                   it->watchdogFeedInterval);
                    }
                }
                
                it->lastWatchdogFeed = currentTime;
                break;
            }
        }
        xSemaphoreGive(taskListMutex);
    }
    
    return true;
}

bool TaskManager::registerCurrentTaskWithWatchdog(const std::string& taskName,
                                                  const WatchdogConfig& config) {
    TaskHandle_t handle = xTaskGetCurrentTaskHandle();
    registerTask(taskName, handle);
    return configureTaskWatchdog(taskName, config);
}

bool TaskManager::configureTaskWatchdog(const std::string& taskName, const WatchdogConfig& config) {
    if (!watchdogInitialized && config.enableWatchdog) {
        TASKM_LOG_W( "Watchdog not initialized, attempting to initialize now");

        // Try to initialize with default settings
        if (!initWatchdog(30, true)) {
            TASKM_LOG_E( "Failed to initialize watchdog for task configuration");
            return false;
        }
    }

    if (xSemaphoreTake(taskListMutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    bool result = false;
    for (std::vector<TaskInfo>::iterator it = taskList_.begin(); it != taskList_.end(); ++it) {
        if (it->taskName == taskName) {
            result = configureTaskWatchdogInternal(*it, config);
            break;
        }
    }

    xSemaphoreGive(taskListMutex);
    return result;
}

bool TaskManager::configureTaskWatchdogInternal(TaskInfo& task, const WatchdogConfig& config) {
    if (!task.taskHandle) {
        return false;
    }

    if (config.enableWatchdog) {
        // Add task to watchdog
        esp_err_t err = esp_task_wdt_add(task.taskHandle);
        if (err == ESP_OK) {
            TASKM_LOG_I("Task %s added to watchdog (critical: %s, interval: %lums)",
                task.taskName.c_str(), config.criticalTask ? "yes" : "no", (unsigned long)config.feedIntervalMs);
        } else if (err == ESP_ERR_INVALID_ARG) {
            TASKM_LOG_I(
                       "Task %s already added to watchdog (critical: %s, interval: %lums)",
                       task.taskName.c_str(), config.criticalTask ? "yes" : "no",
                       (unsigned long)config.feedIntervalMs);
        } else {
            TASKM_LOG_E( "Failed to add task %s to watchdog: %d",
                       task.taskName.c_str(), err);
            return false;
        }

        task.watchdogEnabled = true;
        task.watchdogCritical = config.criticalTask;
        task.watchdogFeedInterval =
            config.feedIntervalMs > 0 ? config.feedIntervalMs : (watchdogTimeoutSeconds * 1000 / 5);
        task.lastWatchdogFeed = xTaskGetTickCount();
        task.watchdogMissedFeeds = 0;
        task.totalWatchdogFeeds = 0;

        // TASKM_LOG_I( "Task %s added to watchdog (critical: %s, interval: %lums)",
        //            task.taskName.c_str(), config.criticalTask ? "yes" : "no",
        //            (unsigned long)task.watchdogFeedInterval);
    } else {
        // Remove task from watchdog
        esp_err_t err = esp_task_wdt_delete(task.taskHandle);
        if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) {  // INVALID_ARG means not subscribed
            TASKM_LOG_E( "Failed to remove task %s from watchdog: %d",
                       task.taskName.c_str(), err);
            return false;
        }

        task.watchdogEnabled = false;
        TASKM_LOG_I( "Task %s removed from watchdog", task.taskName.c_str());
    }

    return true;
}

void TaskManager::watchdogMonitorTask() {
    TASKM_LOG_I( "Watchdog monitor task started");

    // Add ourselves to the watchdog with error handling
    esp_err_t err = esp_task_wdt_add(nullptr);
    if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) {
        TASKM_LOG_E( "Failed to add monitor task to watchdog: %d", err);
    }

    // Initial delay to let system stabilize
    vTaskDelay(pdMS_TO_TICKS(1000));

    while (true) {
        // Feed our own watchdog
        esp_task_wdt_reset();

        // Only check task status if we have tasks to check
        if (xSemaphoreTake(taskListMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (!taskList_.empty()) {
                checkTaskWatchdogStatus();
            }
            xSemaphoreGive(taskListMutex);
        }

        // Wait for next check interval
        vTaskDelay(watchdogCheckInterval);
    }
}

void TaskManager::checkTaskWatchdogStatus() {
    if (xSemaphoreTake(taskListMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    for (std::vector<TaskInfo>::iterator it = taskList_.begin(); it != taskList_.end(); ++it) {
        if (!it->watchdogEnabled || !it->taskHandle) {
            continue;
        }

        // Check if task still exists
        if (eTaskGetState(it->taskHandle) == eDeleted) {
            it->watchdogEnabled = false;
            continue;
        }

        // Note: We can't directly check when other tasks last fed their watchdog
        // This is a monitoring/statistics function only
        // The actual watchdog timeout is handled by ESP-IDF

        // We can track statistics if tasks report their feeds through a shared mechanism
        // For now, we'll just log warnings for critical tasks
        if (it->watchdogCritical) {
            // Could check task state for signs of problems
            eTaskState state = eTaskGetState(it->taskHandle);
            if (state == eSuspended) {
                TASKM_LOG_W(
                           "Critical task %s is suspended - may trigger watchdog!",
                           it->taskName.c_str());
            }
        }
    }

    xSemaphoreGive(taskListMutex);
}

void TaskManager::handleCriticalWatchdogFailure(const TaskInfo& task) {
    TASKM_LOG_E(
               "CRITICAL: Task %s has failed watchdog checks. System will reset.",
               task.taskName.c_str());

    // Log final system state
    logWatchdogStats();
    printExtensiveResourceUsage();

    // Give time for logs to be sent
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Trigger system reset
    esp_restart();
}

bool TaskManager::getTaskWatchdogStats(const std::string& taskName, uint32_t& missedFeeds,
                                       uint32_t& totalFeeds) const {
    if (xSemaphoreTake(taskListMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }

    bool found = false;
    for (std::vector<TaskInfo>::const_iterator it = taskList_.begin(); it != taskList_.end();
         ++it) {
        if (it->taskName == taskName && it->watchdogEnabled) {
            missedFeeds = it->watchdogMissedFeeds;
            totalFeeds = it->totalWatchdogFeeds;
            found = true;
            break;
        }
    }

    xSemaphoreGive(taskListMutex);
    return found;
}

void TaskManager::logWatchdogStats() const {
    if (!watchdogInitialized) {
        TASKM_LOG_I( "Watchdog not initialized");
        return;
    }

    TASKM_LOG_I( "=== Watchdog Statistics ===");
    TASKM_LOG_I( "Timeout: %lu seconds", (unsigned long)watchdogTimeoutSeconds);
    TASKM_LOG_I( "Check interval: %lums", (unsigned long)pdTICKS_TO_MS(watchdogCheckInterval));

    if (xSemaphoreTake(taskListMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        TASKM_LOG_E( "Failed to acquire mutex for watchdog stats");
        return;
    }

    uint32_t totalWatchdogTasks = 0;
    uint32_t criticalTasks = 0;

    for (std::vector<TaskInfo>::const_iterator it = taskList_.begin(); it != taskList_.end();
         ++it) {
        // Only show tasks that are CURRENTLY registered with watchdog
        if (it->watchdogEnabled && it->taskHandle != nullptr) {
            // Extra check: verify task still exists
            eTaskState state = eTaskGetState(it->taskHandle);
            if (state != eInvalid && state != eDeleted) {
                totalWatchdogTasks++;
                if (it->watchdogCritical) {
                    criticalTasks++;
                }

                TASKM_LOG_I( "  %s: Critical=%s, Interval=%lums, Feeds=%lu, Missed=%lu", 
                           it->taskName.c_str(),
                           it->watchdogCritical ? "YES" : "NO", 
                           (unsigned long)it->watchdogFeedInterval,
                           (unsigned long)it->totalWatchdogFeeds,
                           (unsigned long)it->watchdogMissedFeeds);
            }
        }
    }

    TASKM_LOG_I( "Summary: %lu tasks monitored (%lu critical)", (unsigned long)totalWatchdogTasks,
               (unsigned long)criticalTasks);

    xSemaphoreGive(taskListMutex);
    TASKM_LOG_I( "=========================");
}

// /**
//  * @brief Starts a new FreeRTOS task without parameters.
//  *
//  * This method creates and starts a generic FreeRTOS task. It's suitable for tasks that do not
//  * require parameters and are not pinned to a specific core. This overload provides the option to
//  * retrieve the task handle.
//  *
//  * @param taskFunction   Function pointer to the task function. The function should adhere to the
//  * signature `void func(void*)`.
//  * @param taskName       A descriptive name for the task, primarily used for debugging and logging
//  * purposes.
//  * @param stackSize      The stack size in bytes for the task. It should be large enough to handle
//  * the task's local variables and function calls.
//  * @param priority       The priority at which the task should run. Higher numbers represent higher
//  * priorities.
//  * @param abbreviatedName An optional abbreviated name for the task. If left empty, a default
//  * abbreviation based on `taskName` is generated.
//  * @param outTaskHandle  Optional pointer to store the created task's handle. Set to `nullptr` if
//  * the task handle is not needed.
//  *
//  * @return true if the task is successfully created, false if the task creation fails.
//  *
//  * Usage Example:
//  * @code
//  *     TaskManager taskManager;
//  *     TaskHandle_t handle;
//  *     taskManager.startTask(myTaskFunction, "MyTask", 1024, 3, "MTK", &handle);
//  * @endcode
//  *
//  * Note: Ensure that the stack size and priority are set appropriately for the intended task
//  * functionality.
//  */
// bool TaskManager::startTask(void (*taskFunction)(void*), const std::string& taskName,
//                             uint16_t stackSize, UBaseType_t priority,
//                             const std::string& abbreviatedName, TaskHandle_t* outTaskHandle,
//                             const WatchdogConfig& watchdogConfig) {
//     // Dynamically allocate a TaskParams structure using std::make_unique
//     auto params = std::make_unique<TaskParams>(taskFunction, nullptr);

//     // Attempt to create the task with the provided parameters
//     TaskHandle_t taskHandle = nullptr;

//     BaseType_t taskCreated =
//         xTaskCreate(generalTaskWrapper,                // Wrapper function for the FreeRTOS task
//                     taskName.c_str(),                  // Human-readable name of the task
//                     stackSize,                         // Stack size
//                     static_cast<void*>(params.get()),  // Pass raw pointer to TaskParams
//                     priority,                          // Task priority
//                     &taskHandle                        // Task handle
//         );

//     // Error Handling
//     if (taskCreated != pdPASS) {
//         std::string errorMessage;
//         if (taskCreated == errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY) {
//             errorMessage = "Failed to create task: Could not allocate required memory";
//         } else {
//             errorMessage = "Failed to create task: Unknown error";
//         }

//         TASKM_LOG_E( "%s", errorMessage.c_str());
//         return false;  // params is automatically cleaned up by std::unique_ptr
//     }

//     // Transfer ownership to FreeRTOS
//     params.release();

//     // If the caller provided a TaskHandle_t pointer, set it
//     if (outTaskHandle != nullptr) {
//         *outTaskHandle = taskHandle;
//     }

//     // Task Creation Success Logic
//     TaskInfo taskInfo;
//     taskInfo.taskHandle = taskHandle;
//     taskInfo.taskName = taskName;
//     taskInfo.abbreviatedName =
//         abbreviatedName.empty() ? generateAbbreviatedName(taskName, "") : abbreviatedName;
//     taskInfo.startTime = xTaskGetTickCount();
//     taskInfo.coreID = tskNO_AFFINITY;  // Use the correct member name
//     taskInfo.isPinned = false;
//     taskInfo.totalStackSize = stackSize;
//     taskInfo.lastKnownState = eReady;
//     taskInfo.lastStateChangeTick = xTaskGetTickCount();

//     // Initialize watchdog fields
//     taskInfo.watchdogEnabled = false;
//     taskInfo.watchdogCritical = false;
//     taskInfo.watchdogFeedInterval = 0;
//     taskInfo.lastWatchdogFeed = 0;
//     taskInfo.watchdogMissedFeeds = 0;
//     taskInfo.totalWatchdogFeeds = 0;

//     // Protect task list modification with a mutex and add the task info to the list
//     addTaskToList(taskInfo);

//     // Configure watchdog if requested
//     if (watchdogConfig.enableWatchdog && watchdogInitialized) {
//         if (!configureTaskWatchdog(taskName, watchdogConfig)) {
//             TASKM_LOG_W( "Failed to configure watchdog for task %s",
//                        taskName.c_str());
//         }
//     }

//     return true;
// }

void TaskManager::registerTask(const std::string& taskName, TaskHandle_t handle) {
    if (!handle || taskName.empty()) {
        TASKM_LOG_E( "Invalid arguments for registerTask");
        return;
    }

    TaskInfo taskInfo;
    taskInfo.taskHandle = handle;
    taskInfo.taskName = taskName;
    taskInfo.abbreviatedName = generateAbbreviatedName(taskName, "");
    taskInfo.startTime = xTaskGetTickCount();
    taskInfo.coreID = tskNO_AFFINITY;
    taskInfo.isPinned = false;
    taskInfo.totalStackSize = 0;
    taskInfo.lastKnownState = eReady;
    taskInfo.lastStateChangeTick = xTaskGetTickCount();
    taskInfo.watchdogEnabled = false;
    taskInfo.watchdogCritical = false;
    taskInfo.watchdogFeedInterval = 0;
    taskInfo.lastWatchdogFeed = 0;
    taskInfo.watchdogMissedFeeds = 0;
    taskInfo.totalWatchdogFeeds = 0;

    addTaskToList(taskInfo);
}

// /**
//  * @brief Starts a new FreeRTOS task with the specified parameters.
//  *
//  * This method creates and starts a generic FreeRTOS task. It is suitable for tasks that require
//  * parameters and are not pinned to a specific core. This method allows for retrieving the task
//  * handle if needed.
//  *
//  * @param taskFunction   Function pointer to the task function. The function should adhere to the
//  * signature `void func(void*)`.
//  * @param taskName       A descriptive name for the task, primarily used for debugging and logging
//  * purposes.
//  * @param stackSize      The stack size in bytes for the task. It should be large enough to handle
//  * the task's local variables and function calls.
//  * @param parameter      Pointer to the parameter to pass to the task function. Use `nullptr` if no
//  * parameter is needed.
//  * @param priority       The priority at which the task should run. Higher numbers represent higher
//  * priorities.
//  * @param abbreviatedName An optional abbreviated name for the task. If left empty, a default
//  * abbreviation based on `taskName` is generated.
//  * @param outTaskHandle  Optional pointer to store the created task's handle. Set to `nullptr` if
//  * the task handle is not needed.
//  *
//  * @return true if the task is successfully created, false if the task creation fails.
//  *
//  * Usage Example:
//  * @code
//  *     TaskManager taskManager;
//  *     TaskHandle_t handle;
//  *     taskManager.startTask(myTaskFunction, "MyTask", 1024, &param, 3, "MTK", &handle);
//  * @endcode
//  *
//  * Note: The created task will be managed by the FreeRTOS scheduler and can run concurrently with
//  * other tasks. Ensure that the stack size and priority are set appropriately for the intended task
//  * functionality.
//  */
// bool TaskManager::startTask(void (*taskFunction)(void*), const std::string& taskName,
//                             uint16_t stackSize, void* parameter, UBaseType_t priority,
//                             const std::string& abbreviatedName, TaskHandle_t* outTaskHandle,
//                             const WatchdogConfig& watchdogConfig) {
//     // Create a TaskParams object with the task function and parameter
//     TaskParams* params = new TaskParams{taskFunction, parameter};

//     // Generate the FreeRTOS name (truncated to configMAX_TASK_NAME_LEN - 1)
//     std::string freertosName = taskName.substr(0, configMAX_TASK_NAME_LEN - 1);
    
//     // Attempt to create the task with the provided parameters
//     TaskHandle_t taskHandle = nullptr;

//     BaseType_t taskCreated =
//         xTaskCreate(generalTaskWrapper,     // Wrapper function for adapting the function pointer
//                     freertosName.c_str(),   // Use truncated name for FreeRTOS
//                     stackSize,              // Stack size in bytes
//                     static_cast<void*>(params),  // Cast the TaskParams object to a void pointer
//                     priority,                    // Task priority
//                     &taskHandle                  // Task handle to keep track of the created task
//         );

//     // Error Handling
//     if (taskCreated != pdPASS) {
//         std::string errorMessage;
//         switch (taskCreated) {
//             case errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY:
//                 errorMessage = "Failed to create task: Could not allocate required memory";
//                 break;
//             default:
//                 errorMessage = "Failed to create task: Unknown error";
//                 break;
//         }
//         TASKM_LOG_E( "%s", errorMessage.c_str());
//         delete params;  // Clean up the TaskParams object on failure
//         return false;
//     }

//     // Task Creation Success Logic
//     std::string abbreviation =
//         abbreviatedName.empty() ? generateAbbreviatedName(taskName, "") : abbreviatedName;

//     // Determine log tag (use abbreviation if not specified differently)
//     std::string logTag = abbreviation;

//     // Create TaskInfo with all name variants
//     TaskInfo taskInfo = {
//         taskHandle,           // Task handle
//         taskName,             // Full task name (e.g., "ModbusStatusTask")
//         freertosName,         // FreeRTOS name (e.g., "ModbusStatusTas")
//         abbreviation,         // Abbreviated name (e.g., "MbStat")
//         logTag,               // Log tag (e.g., "MbStat")
//         xTaskGetTickCount(),  // Store the start time
//         tskNO_AFFINITY,       // Default core ID (not pinned to a specific core)
//         false,                // Indicates that the task is not pinned
//         stackSize,            // Initial stack size
//         eReady,               // Initial task state
//         xTaskGetTickCount(),  // Initial state change timestamp
//         // Watchdog fields
//         false,  // watchdogEnabled
//         false,  // watchdogCritical
//         0,      // watchdogFeedInterval
//         0,      // lastWatchdogFeed
//         0,      // watchdogMissedFeeds
//         0       // totalWatchdogFeeds
//     };

//     // Log task creation with all name information
//     TASKM_LOG_I( 
//                "Task created - Full: %s, FreeRTOS: %s, Abbrev: %s, LogTag: %s",
//                taskInfo.taskName.c_str(), 
//                taskInfo.freertosName.c_str(),
//                taskInfo.abbreviatedName.c_str(),
//                taskInfo.logTag.c_str());

//     // Store the task handle in the provided pointer, if available
//     if (outTaskHandle != nullptr) {
//         *outTaskHandle = taskHandle;
//     }

//     // Protect task list modification with a mutex and add the task info to the list
//     addTaskToList(taskInfo);

//     // Configure watchdog if requested
//     if (watchdogConfig.enableWatchdog && watchdogInitialized) {
//         if (!configureTaskWatchdog(taskName, watchdogConfig)) {
//             TASKM_LOG_W( "Failed to configure watchdog for task %s",
//                        taskName.c_str());
//         }
//     }

//     return true;
// }

// Enhanced version using TaskNames struct
bool TaskManager::startTask(void (*taskFunction)(void*), 
                           const TaskNames& names,
                           uint16_t stackSize, 
                           void* parameter, 
                           UBaseType_t priority,
                           const WatchdogConfig& watchdogConfig) {
    // Input validation
    if (!taskFunction) {
        TASKM_LOG_E( "Task function cannot be null");
        return false;
    }
    if (names.fullName.empty()) {
        TASKM_LOG_E( "Task name cannot be empty");
        return false;
    }
    if (stackSize < configMINIMAL_STACK_SIZE) {
        TASKM_LOG_E( "Stack size %lu is below minimum %lu", (unsigned long)stackSize, (unsigned long)configMINIMAL_STACK_SIZE);
        return false;
    }
    if (priority >= configMAX_PRIORITIES) {
        TASKM_LOG_E( "Priority %lu exceeds maximum %lu", (unsigned long)priority, (unsigned long)(configMAX_PRIORITIES - 1));
        return false;
    }
    
    // Create a TaskParams object with the task function and parameter
    TaskParams* params = new TaskParams{taskFunction, parameter};
    
    // Attempt to create the task with the provided parameters
    TaskHandle_t taskHandle = nullptr;

    BaseType_t taskCreated =
        xTaskCreate(generalTaskWrapper,           // Wrapper function for adapting the function pointer
                    names.freertosName.c_str(),   // Use the FreeRTOS-safe name
                    stackSize,                    // Stack size in bytes
                    static_cast<void*>(params),   // Cast the TaskParams object to a void pointer
                    priority,                     // Task priority
                    &taskHandle                   // Task handle to keep track of the created task
        );

    // Error Handling
    if (taskCreated != pdPASS) {
        std::string errorMessage;
        switch (taskCreated) {
            case errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY:
                errorMessage = "Failed to create task: Could not allocate required memory";
                break;
            default:
                errorMessage = "Failed to create task: Unknown error";
                break;
        }
        TASKM_LOG_E( "Failed to create task '%s': %s", 
                   names.fullName.c_str(), errorMessage.c_str());
        delete params;  // Clean up the TaskParams object on failure
        return false;
    }

    // Create TaskInfo with all name variants from TaskNames
    TaskInfo taskInfo = {
        taskHandle,                // Task handle
        names.fullName,            // Full task name
        names.freertosName,        // FreeRTOS name (truncated)
        names.abbreviation,        // Abbreviated name
        names.logTag,              // Log tag
        xTaskGetTickCount(),       // Store the start time
        tskNO_AFFINITY,           // Default core ID (not pinned to a specific core)
        false,                     // Indicates that the task is not pinned
        stackSize,                 // Initial stack size
        eReady,                    // Initial task state
        xTaskGetTickCount(),       // Initial state change timestamp
        // Watchdog fields
        false,  // watchdogEnabled
        false,  // watchdogCritical
        0,      // watchdogFeedInterval
        0,      // lastWatchdogFeed
        0,      // watchdogMissedFeeds
        0       // totalWatchdogFeeds
    };

    // Log task creation with all name information
    TASKM_LOG_I( 
               "Task created - Full: %s, FreeRTOS: %s, Abbrev: %s, LogTag: %s",
               taskInfo.taskName.c_str(), 
               taskInfo.freertosName.c_str(),
               taskInfo.abbreviatedName.c_str(),
               taskInfo.logTag.c_str());

    // Protect task list modification with a mutex and add the task info to the list
    addTaskToList(taskInfo);

    // Configure watchdog if requested
    if (watchdogConfig.enableWatchdog && watchdogInitialized) {
        if (!configureTaskWatchdog(names.fullName, watchdogConfig)) {
            TASKM_LOG_W( "Failed to configure watchdog for task %s",
                       names.fullName.c_str());
        }
    }

    return true;
}

/**
 * @brief Starts a task that executes a member function of the TaskManager class.
 *
 * This method allows for the creation of a FreeRTOS task that runs a specific member function of
 * the TaskManager class. It encapsulates the complexities of creating a FreeRTOS task and managing
 * its lifecycle. The task will execute the specified member function until it is explicitly deleted
 * or the function completes.
 *
 * @param memberFunction Pointer to the member function of TaskManager to be executed by the task.
 *                       This member function must have a void return type and take no parameters.
 * @param taskName       A string representing the name of the task. This name is useful for
 * debugging purposes.
 * @param stackSize      The stack size in bytes for the task. It's important to choose an
 * appropriate size to avoid stack overflow or wastage of memory.
 * @param priority       The priority at which the task should run. Tasks with higher priorities are
 * executed preferentially compared to lower priority tasks.
 * @param abbreviatedName An optional abbreviated name for the task, primarily used for logging.
 *                        If left empty, the first three characters of taskName will be used.
 * @param outTaskHandle  Optional pointer to a variable where the created task's handle will be
 * stored. This can be used to reference or manage the task later. If not needed, pass `nullptr`.
 *
 * @return true if the task was successfully created and added to the task list, false otherwise.
 *
 * Usage Example:
 * @code
 *     TaskManager taskManager;
 *     TaskHandle_t handle;
 *     taskManager.startTask(&TaskManager::memberFunction, "MyTask", 1024, 3, "MTK", &handle);
 *
 *     // Without needing the handle
 *     taskManager.startTask(&TaskManager::memberFunction, "MyTask", 1024, 3, "MTK", nullptr);
 * @endcode
 *
 * Note: The created task will be managed by the FreeRTOS scheduler and can run concurrently with
 * other tasks. Ensure that the stack size and priority are set appropriately for the intended task
 * functionality.
 */
bool TaskManager::startTask(void (TaskManager::*memberFunction)(void), const std::string& taskName,
                            uint16_t stackSize, UBaseType_t priority,
                            const std::string& abbreviatedName, TaskHandle_t* outTaskHandle,
                            const WatchdogConfig& watchdogConfig) {
    // Create a wrapper data object to pass to the FreeRTOS task creation function
    MemberFunctionWrapper* wrapper = new MemberFunctionWrapper{this, memberFunction};

    // Generate the FreeRTOS name (truncated)
    std::string freertosName = taskName.substr(0, configMAX_TASK_NAME_LEN - 1);
    
    // Attempt to create the task with the provided parameters
    TaskHandle_t taskHandle = nullptr;

    BaseType_t taskCreated = xTaskCreate(
        memberFunctionTaskWrapper,    // Wrapper function that adapts the member function for FreeRTOS
        freertosName.c_str(),        // Use truncated name for FreeRTOS
        stackSize,                   // Size of the stack to allocate for the task
        static_cast<void*>(wrapper), // Pass the MemberFunctionWrapper object
        priority,                    // Priority of the task within the FreeRTOS scheduler
        &taskHandle                  // Task handle to keep track of the created task
    );

    // Error Handling
    if (taskCreated != pdPASS) {
        std::string errorMessage;
        switch (taskCreated) {
            case errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY:
                errorMessage = "Failed to create task: Could not allocate required memory";
                break;
            default:
                errorMessage = "Failed to create task: Unknown error";
                break;
        }
        TASKM_LOG_E( "%s", errorMessage.c_str());
        delete wrapper;  // Delete the wrapper if task creation failed
        return false;
    }

    // If the caller provided a TaskHandle_t pointer, set it
    if (outTaskHandle != nullptr) {
        *outTaskHandle = taskHandle;
    }

    // Task Creation Success Logic
    std::string abbreviation =
        abbreviatedName.empty() ? generateAbbreviatedName(taskName, "") : abbreviatedName;
    
    // Determine log tag (use abbreviation if not specified differently)
    std::string logTag = abbreviation;

    // Add the task info to the task list
    TaskInfo taskInfo = {
        taskHandle,           // Task handle
        taskName,             // Full task name
        freertosName,         // FreeRTOS name (truncated)
        abbreviation,         // Abbreviated name
        logTag,               // Log tag
        xTaskGetTickCount(),  // Store the start time
        tskNO_AFFINITY,       // Default core ID (not pinned to a specific core)
        false,                // Indicates that the task is not pinned
        stackSize,            // Initial stack size (in words)
        eReady,               // Initial task state
        xTaskGetTickCount(),  // Initial state change timestamp
        // Watchdog fields
        false,  // watchdogEnabled
        false,  // watchdogCritical
        0,      // watchdogFeedInterval
        0,      // lastWatchdogFeed
        0,      // watchdogMissedFeeds
        0       // totalWatchdogFeeds
    };

    // Log task creation with all name information
    TASKM_LOG_I( 
               "Task created - Full: %s, FreeRTOS: %s, Abbrev: %s, LogTag: %s",
               taskInfo.taskName.c_str(), 
               taskInfo.freertosName.c_str(),
               taskInfo.abbreviatedName.c_str(),
               taskInfo.logTag.c_str());

    // Protect task list modification with a mutex and add the task info to the list
    addTaskToList(taskInfo);

    // Configure watchdog if requested
    if (watchdogConfig.enableWatchdog && watchdogInitialized) {
        if (!configureTaskWatchdog(taskName, watchdogConfig)) {
            TASKM_LOG_W( "Failed to configure watchdog for task %s",
                       taskName.c_str());
        }
    }

    // The wrapper will automatically clean up when it goes out of scope
    return true;
}

/**
 * @brief Starts a new task pinned to a specific CPU core in an ESP32 environment.
 *
 * This method is used to create and start a FreeRTOS task that is pinned (or locked) to a specific
 * core on the ESP32. Pinned tasks are guaranteed to run on the specified core, which can be
 * beneficial for performance reasons, especially in a multi-core environment like the ESP32. This
 * overload also allows retrieving the task handle.
 *
 * @param taskFunction   Function pointer to the task function. This function should match the
 * signature `void func(void*)`.
 * @param taskName       A descriptive name for the task, used in debugging.
 * @param stackSize      The stack size in bytes for the task. This needs to be sufficient to hold
 * the task's local variables and function calls.
 * @param parameter      A pointer to a variable to be passed to the task function. Can be `nullptr`
 * if no parameter is needed.
 * @param priority       The priority at which the task should run. Higher values indicate higher
 * priority.
 * @param coreID         The ID of the core to which the task should be pinned. Typically `0` or `1`
 * for ESP32.
 * @param abbreviatedName An optional short name for logging and tracking purposes. If left empty, a
 * default will be generated based on `taskName`.
 * @param outTaskHandle  Optional pointer to store the created task's handle. Set to `nullptr` if
 * the task handle is not needed.
 *
 * @return true if the task is successfully created and pinned to the specified core, false if the
 * task creation fails.
 *
 * Usage Example:
 * @code
 *     TaskManager taskManager;
 *     TaskHandle_t handle;
 *     taskManager.startTaskPinned(myTaskFunction, "MyPinnedTask", 2048, nullptr, 5, 1, "MPT",
 * &handle);
 *
 *     // Without needing the handle
 *     taskManager.startTaskPinned(myTaskFunction, "MyPinnedTask", 2048, nullptr, 5, 1, "MPT",
 * nullptr);
 * @endcode
 *
 * Note: Pinning tasks to a core can help with real-time processing requirements but may lead to
 * underutilization of the other core. Use this feature judiciously to maintain system balance.
 */
bool TaskManager::startTaskPinned(void (*taskFunction)(void*), const std::string& taskName,
                                  uint16_t stackSize, void* parameter, UBaseType_t priority,
                                  BaseType_t coreID, const std::string& abbreviatedName,
                                  TaskHandle_t* outTaskHandle,
                                  const WatchdogConfig& watchdogConfig) {
    // Create a TaskParams object with the task function and parameter
    TaskParams* params = new TaskParams{taskFunction, parameter};

    // Generate the FreeRTOS name (truncated)
    std::string freertosName = taskName.substr(0, configMAX_TASK_NAME_LEN - 1);

    // Attempt to create the task with the provided parameters
    TaskHandle_t taskHandle = nullptr;

    BaseType_t taskCreated = xTaskCreatePinnedToCore(
        generalTaskWrapper,          // Wrapper function that adapts the function pointer for FreeRTOS
        freertosName.c_str(),       // Use truncated name
        stackSize,                  // Size of the stack to allocate for the task
        static_cast<void*>(params), // Cast the TaskParams object to a void pointer for the task
        priority,                   // Priority of the task within the FreeRTOS scheduler
        &taskHandle,                // Task handle to keep track of the created task
        coreID                      // Core ID where the task should run
    );

    // Error Handling
    if (taskCreated != pdPASS) {
        std::string errorMessage;
        switch (taskCreated) {
            case errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY:
                errorMessage = "Failed to pin and create task: Could not allocate required memory";
                break;
            default:
                errorMessage = "Failed to pin and create task: Unknown error";
                break;
        }
        TASKM_LOG_E( "%s", errorMessage.c_str());
        delete params;  // Clean up the TaskParams object on failure
        return false;
    }

    // If the caller provided a TaskHandle_t pointer, set it
    if (outTaskHandle != nullptr) {
        *outTaskHandle = taskHandle;
    }

    // Task Creation Success Logic
    std::string abbreviation =
        abbreviatedName.empty() ? generateAbbreviatedName(taskName, "") : abbreviatedName;
    
    std::string logTag = abbreviation;

    // Add the task info to the task list, ensuring thread-safe access
    TaskInfo taskInfo = {
        taskHandle,           // Task handle to manage the task
        taskName,             // Human-readable task name
        freertosName,         // FreeRTOS name (truncated)
        abbreviation,         // Shortened name for easy identification
        logTag,               // Log tag
        xTaskGetTickCount(),  // Capture the start time for tracking task runtime
        coreID,               // Core ID for task affinity, indicating pinned core
        true,                 // Flag to indicate that the task is pinned to a core
        stackSize,            // Size of the stack allocated for the task
        eReady,               // Initial task state
        xTaskGetTickCount(),  // Initial state change timestamp
        // Watchdog fields
        false,  // watchdogEnabled
        false,  // watchdogCritical
        0,      // watchdogFeedInterval
        0,      // lastWatchdogFeed
        0,      // watchdogMissedFeeds
        0       // totalWatchdogFeeds
    };

    // Log task creation
    TASKM_LOG_I( 
               "Pinned task created - Full: %s, FreeRTOS: %s, Core: %d",
               taskInfo.taskName.c_str(), 
               taskInfo.freertosName.c_str(),
               coreID);

    // Protect task list modification with a mutex and add the task info to the list
    addTaskToList(taskInfo);

    // Configure watchdog if requested
    if (watchdogConfig.enableWatchdog && watchdogInitialized) {
        if (!configureTaskWatchdog(taskName, watchdogConfig)) {
            TASKM_LOG_W( "Failed to configure watchdog for task %s",
                       taskName.c_str());
        }
    }

    return true;
}

/**
 * @brief Gets a list of all tasks managed by this instance.
 */
const std::vector<TaskManager::TaskInfo>& TaskManager::getTaskList() const noexcept { return taskList_; }

/**
 * @brief Sets the interval for task execution monitoring.
 * @param interval_ms The interval in milliseconds.
 */
void TaskManager::setTaskExecutionInterval(uint32_t interval_ms) {
    // Convert the interval from milliseconds to FreeRTOS ticks
    taskExecutionInterval = pdMS_TO_TICKS(interval_ms);
    TASKM_LOG_I( "Task execution interval set to %lu ms",
               (unsigned long)interval_ms);
}

/**
 * @brief Sets the interval for resource logging.
 * @param interval_ms The interval in milliseconds.
 */
void TaskManager::setResourceLogPeriod(uint32_t interval_ms) {
    // Convert the interval from milliseconds to FreeRTOS ticks
    resourceLogPeriod = pdMS_TO_TICKS(interval_ms);

    // Log the updated resource log period using the global logger
    TASKM_LOG_I( "Resource log period set to %u ms",
               static_cast<unsigned int>(interval_ms));
}

/**
 * @brief Debug task that logs task, system information, and runtime statistics periodically.
 */
void TaskManager::debugTask() {
    TASKM_LOG_I( "Debug task started");

    // Wait for task registration to complete
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Check if this task should have watchdog based on its config
    bool debugTaskWatchdogEnabled = false;
    uint32_t watchdogFeedInterval = 0;
    
    // Find this task in the task list to check its watchdog config
    if (xSemaphoreTake(taskListMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (const auto& task : taskList_) {
            if (task.taskName == "DebugTask") {
                debugTaskWatchdogEnabled = task.watchdogEnabled;
                watchdogFeedInterval = task.watchdogFeedInterval;
                TASKM_LOG_I( "Debug task watchdog config: enabled=%s, interval=%u ms",
                          debugTaskWatchdogEnabled ? "true" : "false", watchdogFeedInterval);
                break;
            }
        }
        xSemaphoreGive(taskListMutex);
    }
    
    // If watchdog was not configured during task creation, log it
    if (!debugTaskWatchdogEnabled) {
        TASKM_LOG_I( "Debug task running without watchdog monitoring");
    }
    
    // Log the initial FreeRTOS scheduler state
    const char* schedulerState = schedulerStateToString(xTaskGetSchedulerState());
    TASKM_LOG_I( "FreeRTOS scheduler state: %s", schedulerState);

    TickType_t lastResourceLogTime = xTaskGetTickCount();
    TickType_t lastWatchdogStatsTime = xTaskGetTickCount();
    TickType_t lastWatchdogFeedTime = xTaskGetTickCount();
    std::unordered_map<std::string, eTaskState> lastTaskStates;
    
    // ADD THIS: Counter for periodic cleanup
    int cleanupCounter = 0;

    while (true) {
        TickType_t loopStart = xTaskGetTickCount();
        
        // Feed watchdog if enabled and interval has elapsed
        if (debugTaskWatchdogEnabled) {
            TickType_t timeSinceLastFeed = xTaskGetTickCount() - lastWatchdogFeedTime;
            if (timeSinceLastFeed >= pdMS_TO_TICKS(watchdogFeedInterval)) {
                esp_task_wdt_reset();
                lastWatchdogFeedTime = xTaskGetTickCount();
            }
        }
        
        // Process task state changes
        TaskStateMessage stateMessage;  // Will use constructor for initialization

        // Process task state change notifications
        while (xQueueReceive(taskStateQueue, &stateMessage, 0) == pdPASS) {
            // Skip invalid or debug task messages
            if (stateMessage.taskName[0] == '\0' ||
                strcmp(stateMessage.taskName, "DebugTask") == 0) {
                continue;
            }

            // Process state changes for other tasks
            std::string taskNameStr(stateMessage.taskName);
            std::unordered_map<std::string, eTaskState>::iterator it =
                lastTaskStates.find(taskNameStr);

            // Debounce task state changes
            if (it == lastTaskStates.end() || it->second != stateMessage.state) {
                lastTaskStates[taskNameStr] = stateMessage.state;
                TASKM_LOG_D( "Task %s changed state to %s at tick %u",
                           stateMessage.taskName, taskStateToStringShort(stateMessage.state),
                           stateMessage.timestamp);
            }
        }

        // Log resource usage periodically
        if ((xTaskGetTickCount() - lastResourceLogTime) >= resourceLogPeriod) {
            if (xSemaphoreTake(taskListMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                updateAndLogTaskStates();
                xSemaphoreGive(taskListMutex);

                // Print resource usage separately to minimize mutex hold time
                printExtensiveResourceUsage();
                lastResourceLogTime = xTaskGetTickCount();
            }
        }

        // ADD THIS: Periodically clean up deleted tasks
        if (++cleanupCounter >= 10) {  // Every 10 iterations (1 second with 100ms interval)
            cleanupCounter = 0;
            
            // Get task counts before cleanup
            int validBefore = 0, deletedBefore = 0;
            getTaskCounts(validBefore, deletedBefore);
            
            if (deletedBefore > 0) {
                TASKM_LOG_D( "Cleaning up %d deleted tasks", deletedBefore);
                cleanupDeletedTasks();
                
                // Log results after cleanup
                int validAfter = 0, deletedAfter = 0;
                getTaskCounts(validAfter, deletedAfter);
                TASKM_LOG_D( "Cleanup complete: %d tasks remain", validAfter);
            }
        }

        // Log watchdog stats periodically
        if (watchdogInitialized &&
            (xTaskGetTickCount() - lastWatchdogStatsTime) >= pdMS_TO_TICKS(60000)) {
            logWatchdogStats();
            lastWatchdogStatsTime = xTaskGetTickCount();
        }

        // Feed watchdog before delay if we're close to timeout
        if (debugTaskWatchdogEnabled) {
            TickType_t timeSinceLastFeed = xTaskGetTickCount() - lastWatchdogFeedTime;
            // Feed if we're within 20% of the interval to be safe
            if (timeSinceLastFeed >= pdMS_TO_TICKS(watchdogFeedInterval * 0.8)) {
                esp_task_wdt_reset();
                lastWatchdogFeedTime = xTaskGetTickCount();
            }
        }

        // Delay for remaining interval
        TickType_t loopDuration = xTaskGetTickCount() - loopStart;
        if (loopDuration < taskExecutionInterval) {
            vTaskDelay(taskExecutionInterval - loopDuration);
        } else {
            TASKM_LOG_W( "Debug task overran interval by %u ticks",
                       loopDuration - taskExecutionInterval);
            vTaskDelay(1);  // Yield at least
        }
    }
}

/**
 * @brief Logs detailed information about a specific task.
 *
 * This function logs the state, priority, runtime, stack usage, and core affinity
 * of a task, identified by the TaskInfo structure.
 *
 * @param task A constant reference to a TaskInfo object representing the task to be logged.
 */
void TaskManager::logTaskInfo(const TaskInfo& task) {
    if (task.taskHandle == nullptr) {
        TASKM_LOG_E( "Invalid task handle for task: %s", task.taskName.c_str());
        return;
    }

    // CRITICAL FIX: Check task state first
    eTaskState taskState = eTaskGetState(task.taskHandle);
    if (taskState == eInvalid || taskState == eDeleted) {
        TASKM_LOG_D( "%s=DELETED", task.abbreviatedName.c_str());
        return;
    }

    // Safe to access other properties now
#ifdef TASKMANAGER_DEBUG
    UBaseType_t prio = uxTaskPriorityGet(task.taskHandle);
    TickType_t currentTick = xTaskGetTickCount();
    TickType_t runTime = currentTick - task.startTime;
    UBaseType_t stackMark = uxTaskGetStackHighWaterMark(task.taskHandle);
    float stackPercentage = task.totalStackSize > 0
                                ? (static_cast<float>(stackMark) / task.totalStackSize) * 100.0f
                                : 0.0f;

    TASKM_LOG_D( "%s=%s(%u),T:%u,Stack:%.2f%%,%s", 
               task.abbreviatedName.c_str(),
               taskStateToStringShort(taskState), prio, runTime, stackPercentage,
               task.isPinned ? "Core:P" : "Core:NP");
#else
    // Variables only needed for debug logging
    (void)task; // Suppress unused parameter warning when debug is disabled
#endif
}

/**
 * @brief General wrapper function for FreeRTOS tasks.
 *
 * This function adapts a generic function pointer to the FreeRTOS task function signature.
 * It safely cleans up dynamically allocated memory for task parameters after execution.
 *
 * @param pvParameters A void pointer, expected to be of type TaskParams, containing the task
 * function and its arguments.
 */
void TaskManager::generalTaskWrapper(void* pvParameters) {
    // Safely cast the parameters to TaskParams
    TaskParams* taskParams = static_cast<TaskParams*>(pvParameters);

    // Validate the task parameters
    if (taskParams == nullptr || taskParams->function == nullptr) {
        TASKM_LOG_E("Invalid task parameters passed to generalTaskWrapper");
        vTaskDelete(nullptr);  // Safely delete the task
        return;
    }

    // Log task execution start
    TASKM_LOG_I("Task function starting: %p", taskParams->function);

    // Call the task function with the provided argument
    taskParams->function(taskParams->arg);

    // Log task completion
    TASKM_LOG_I("Task function completed: %p", taskParams->function);

    // Clean up the allocated TaskParams object
    delete taskParams;

    // Small delay to ensure cleanup completes
    vTaskDelay(pdMS_TO_TICKS(10));

    // Safely delete the task to free its resources
    vTaskDelete(nullptr);
}

/**
 * @brief Wrapper function for the debug task to adapt to FreeRTOS task signature.
 */
void TaskManager::debugTaskWrapper(void* pvParameters) {
    if (pvParameters == nullptr) {
        TASKM_LOG_E( "debugTaskWrapper called with null parameters");
        vTaskDelete(nullptr);
        return;
    }

    TaskManager* instance = static_cast<TaskManager*>(pvParameters);

    // Call the debugTask method of the TaskManager instance
    instance->debugTask();

    // Safely delete the task when finished
    vTaskDelete(nullptr);
}

/**
 * @brief Converts a FreeRTOS task state to a readable string.
 */
std::string TaskManager::taskStateToString(eTaskState taskState) {
    switch (taskState) {
        case eRunning:
            return "Running";
        case eReady:
            return "Ready";
        case eBlocked:
            return "Blocked";
        case eSuspended:
            return "Suspended";
        case eDeleted:
            return "Deleted";
        case eInvalid:
            return "Invalid";
        default:
            return "Unknown";
    }
}

/**
 * @brief Converts a FreeRTOS task state to a short, readable string.
 */
const char* TaskManager::taskStateToStringShort(eTaskState taskState) {
    switch (taskState) {
        case eRunning:
            return "RUN";
        case eReady:
            return "RDY";
        case eBlocked:
            return "BLK";
        case eSuspended:
            return "SUS";
        case eDeleted:
            return "DEL";
        default:
            return "UNK";
    }
}

/**
 * @brief Converts a FreeRTOS scheduler state to a readable string.
 */
const char* TaskManager::schedulerStateToString(int state) {
    switch (state) {
        case taskSCHEDULER_RUNNING:
            return "RUNNING";
        case taskSCHEDULER_SUSPENDED:
            return "SUSPENDED";
        case taskSCHEDULER_NOT_STARTED:
            return "NOT_STARTED";
        default:
            return "UNKNOWN";
    }
}

// Assume mutex 'taskListMutex' is defined and initialized in the class
/**
 * @brief Prints extensive resource usage information for the system and tasks.
 */
void TaskManager::printExtensiveResourceUsage() {
    // Log heap usage
    size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    size_t totalHeap = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    size_t minHeapEver = xPortGetMinimumEverFreeHeapSize();

    TASKM_LOG_I( "Heap: Total=%u, Free=%u, Min Free=%u", totalHeap, freeHeap,
               minHeapEver);

#ifdef ESP32
    // Log PSRAM usage if available
    if (psramFound()) {
        size_t totalPsram = ESP.getPsramSize();
        size_t freePsram = ESP.getFreePsram();
        TASKM_LOG_I( "PSRAM: Total=%u, Free=%u", totalPsram, freePsram);
    }
#endif

    // Protect access to taskList_ with the mutex
    if (xSemaphoreTake(taskListMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        std::vector<TaskInfo>::const_iterator it;
        for (it = taskList_.begin(); it != taskList_.end(); ++it) {
            if (it->taskHandle != nullptr) {
                // EXTRA SAFETY: Validate the handle value itself
                uintptr_t handleValue = (uintptr_t)(it->taskHandle);
                
                // Check for obviously corrupted handles
                if (handleValue < 0x3FFB0000 || handleValue > 0x3FFFFFFF) {
                    TASKM_LOG_D( "Task '%s': BAD HANDLE", 
                              it->taskName.c_str());
                    continue;
                }
                
                // CRITICAL FIX: Check task state first
                eTaskState taskState = eTaskGetState(it->taskHandle);
                
                // Skip deleted or invalid tasks
                if (taskState == eInvalid || taskState == eDeleted) {
                    TASKM_LOG_D( "Task '%s': DELETED", 
                              it->taskName.c_str());
                    continue;
                }
                
                // Only access stack info for tasks in stable states
                if (taskState == eRunning || taskState == eBlocked || taskState == eSuspended) {
                    // Safe to access task properties now
                    UBaseType_t highWaterMark = uxTaskGetStackHighWaterMark(it->taskHandle);
                    UBaseType_t taskPriority = uxTaskPriorityGet(it->taskHandle);

                    // Enhanced logging with watchdog info
                    if (it->watchdogEnabled) {
                        TASKM_LOG_I(
                                   "Task '%s': State=%s, Priority=%u, Stack HWM=%u words, WDT=%s",
                                   it->taskName.c_str(), taskStateToStringShort(taskState),
                                   taskPriority, highWaterMark,
                                   it->watchdogCritical ? "CRITICAL" : "ON");
                    } else {
                        TASKM_LOG_I(
                                   "Task '%s': State=%s, Priority=%u, Stack HWM=%u words",
                                   it->taskName.c_str(), taskStateToStringShort(taskState),
                                   taskPriority, highWaterMark);
                    }
                } else {
                    // Task in eReady state - log without stack info
                    // SAFETY: Don't access priority for potentially deleted tasks
                    if (it->watchdogEnabled) {
                        TASKM_LOG_I(
                                   "Task '%s': State=%s, Priority=?, Stack HWM=?, WDT=%s",
                                   it->taskName.c_str(), taskStateToStringShort(taskState),
                                   it->watchdogCritical ? "CRITICAL" : "ON");
                    } else {
                        TASKM_LOG_I(
                                   "Task '%s': State=%s, Priority=?, Stack HWM=?",
                                   it->taskName.c_str(), taskStateToStringShort(taskState));
                    }
                }
            } else {
                TASKM_LOG_E( "Invalid task handle for task: %s",
                           it->taskName.c_str());
            }
        }
        xSemaphoreGive(taskListMutex);  // Release the mutex
    } else {
        TASKM_LOG_E(
                   "Failed to acquire taskListMutex for printing resource usage");
    }

    // Log heap usage again for clarity
    size_t usedHeap = totalHeap - freeHeap;
    TASKM_LOG_I( "Heap Usage: %u bytes used out of %u bytes", usedHeap, totalHeap);
}

/**
 * @brief Wrapper function for member function tasks.
 */
/**
 * @brief Wrapper function for member function tasks.
 */
void TaskManager::memberFunctionTaskWrapper(void* pvParameters) {
    MemberFunctionWrapper* wrapper = static_cast<MemberFunctionWrapper*>(pvParameters);

    if (wrapper != nullptr && wrapper->getData() != nullptr) {
        MemberFunctionWrapperData* wrapperData = wrapper->getData();
        // Execute the member function
        if (wrapperData->instance != nullptr && wrapperData->memberFunction != nullptr) {
            (wrapperData->instance->*(wrapperData->memberFunction))();
        } else {
            // Log an error if the instance or function pointer is invalid
            TASKM_LOG_E(
                       "Invalid instance or function pointer in memberFunctionTaskWrapper");
        }
        delete wrapper;  // Clean up dynamically allocated memory here
    } else {
        // Log an error if wrapper is null
        TASKM_LOG_E( "Invalid wrapper in memberFunctionTaskWrapper");
    }

    // Clean up the task before exiting
    vTaskDelete(nullptr);
}

void TaskManager::addTaskToList(const TaskInfo& taskInfo) {
    // Validate task info
    if (taskInfo.taskHandle == nullptr || taskInfo.taskName.empty()) {
        TASKM_LOG_E( "Attempted to add invalid task info");
        return;
    }

    // Use RAII mutex guard for thread-safe access
    MutexGuard lock(taskListMutex);
    if (!lock.hasLock()) {
        TASKM_LOG_E( "Failed to acquire mutex");
        return;
    }

    // Check for sufficient memory before adding the task
    if (taskList_.size() >= taskList_.capacity()) {
        // Check if we can grow the vector
        size_t currentSize = taskList_.size();
        if (currentSize >= taskList_.max_size() - 10) {
            TASKM_LOG_E( "Failed to add task '%s': Cannot grow task list",
                       taskInfo.taskName.c_str());
            return;
        }

        size_t newCapacity = currentSize + 10;  // Grow by 10 elements
        taskList_.reserve(newCapacity);

        // Verify the reserve worked
        if (taskList_.capacity() < newCapacity) {
            TASKM_LOG_E( "Failed to add task '%s': Insufficient memory",
                       taskInfo.taskName.c_str());
            return;
        }
    }

    // Add the task info to the list
    taskList_.push_back(taskInfo);
    TASKM_LOG_I( "Task '%s' added successfully", taskInfo.taskName.c_str());
    
    // Mutex automatically released by MutexGuard destructor
}

void TaskManager::updateTaskState(TaskInfo& task) {
    if (task.taskHandle == nullptr) {
        return;
    }

    eTaskState currentState = eTaskGetState(task.taskHandle);
    if (currentState != task.lastKnownState) {
        TASKM_LOG_D( "Task %s changed state from %s to %s at tick %u",
                   task.abbreviatedName.c_str(), taskStateToStringShort(task.lastKnownState),
                   taskStateToStringShort(currentState), xTaskGetTickCount());

        task.lastKnownState = currentState;
        task.lastStateChangeTick = xTaskGetTickCount();
    }
}

void TaskManager::handleTaskStateChange(const std::string& taskName, eTaskState newState) {
    // Lock the task list mutex before accessing the task list
    if (xSemaphoreTake(taskListMutex, portMAX_DELAY) != pdTRUE) {
        TASKM_LOG_E( "Failed to acquire mutex for task state change: %s",
                   taskName.c_str());
        return;
    }

    std::vector<TaskInfo>::iterator it;
    // Iterate through the task list to find the task with the given name
    for (it = taskList_.begin(); it != taskList_.end(); ++it) {
        if (it->taskName == taskName) {
            // Update the task's state and the timestamp of the state change
            it->lastKnownState = newState;
            it->lastStateChangeTick = xTaskGetTickCount();

            // Optionally, log the state change here
            TASKM_LOG_I( "Task %s changed state to %s at tick %u",
                       taskName.c_str(), taskStateToString(newState).c_str(),
                       it->lastStateChangeTick);
            break;
        }
    }
    
    // Mutex automatically released by MutexGuard destructor
}

// Monitor Task Notifications
void TaskManager::monitorTaskNotifications() {
    TaskStateChange stateChange;

    while (true) {
        // Wait indefinitely for a task state change notification
        if (xQueueReceive(taskStateQueue, &stateChange, portMAX_DELAY) == pdPASS) {
            // Handle the received task state change
            handleTaskStateChange(stateChange.taskName, stateChange.newState);

            // Optional: Notify the debug task to update the log immediately
            if (debugTaskHandle != nullptr) {
                xTaskNotify(debugTaskHandle, 0,
                            eNoAction);  // Notify without incrementing the notification value
            }
        }
    }
}

std::string TaskManager::taskNameFromHandle(TaskHandle_t taskHandle) const {
    if (taskHandle == nullptr) {
        return "Invalid Task";
    }

    #if INCLUDE_pcTaskGetName == 1
        const char* taskName = pcTaskGetName(taskHandle);
        return std::string(taskName != nullptr ? taskName : "Unknown Task");
    #else
        // Fallback: Try to find task in our internal registry
        if (xSemaphoreTake(taskListMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            for (const auto& task : taskList_) {
                if (task.taskHandle == taskHandle) {
                    std::string name(task.taskName);
                    xSemaphoreGive(taskListMutex);
                    return name;
                }
            }
            xSemaphoreGive(taskListMutex);
        }
        
        // If not found in registry, return generic name
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "Task_%p", taskHandle);
        return std::string(buffer);
    #endif
}

TaskHandle_t TaskManager::getTaskHandleByName(const std::string& taskName) const noexcept {
    if (xSemaphoreTake(taskListMutex, portMAX_DELAY) != pdTRUE) {
        TASKM_LOG_E( "Failed to acquire mutex while getting task handle for: %s",
                   taskName.c_str());
        return nullptr;
    }

    TaskHandle_t foundHandle = nullptr;
    std::vector<TaskInfo>::const_iterator it;
    for (it = taskList_.begin(); it != taskList_.end(); ++it) {
        if (it->taskName == taskName) {
            foundHandle = it->taskHandle;
            break;
        }
    }

    xSemaphoreGive(taskListMutex);
    return foundHandle;
}

bool TaskManager::notifyTaskStateChange(const TaskStateChange& stateChange) {
    if (taskStateQueue == nullptr) {
        TASKM_LOG_E( "Task state queue not initialized");
        return false;
    }

    TaskStateMessage message;
    strncpy(message.taskName, stateChange.taskName, TaskStateMessage::MAX_TASK_NAME - 1);
    message.taskName[TaskStateMessage::MAX_TASK_NAME - 1] = '\0';
    message.state = stateChange.newState;
    message.timestamp = xTaskGetTickCount();

    // Use a timeout when sending to queue
    if (xQueueSendToBack(taskStateQueue, &message, pdMS_TO_TICKS(10)) != pdPASS) {
        TASKM_LOG_W( "Failed to enqueue state change for task '%s'",
                   message.taskName);
        return false;
    }

    // Log the successful notification
    TASKM_LOG_D(
               "Task state change for task '%s' enqueued successfully: newState=%s.",
               message.taskName, taskStateToStringShort(message.state));

    return true;
}

// Utility method if needed
/**
 * @brief Generates an abbreviated name for a task.
 */
std::string TaskManager::generateAbbreviatedName(const std::string& taskName,
                                                 const std::string& customAbbreviation) {
    // Use custom abbreviation if provided and not empty
    if (!customAbbreviation.empty()) {
        return customAbbreviation;
    }

    std::string abbreviation;
    // Check if taskName is shorter than 3 characters
    if (taskName.length() < 3) {
        abbreviation = taskName;
        // If taskName is too short, pad it with underscores or handle appropriately
        abbreviation.resize(3, '_');
    } else {
        abbreviation = taskName.substr(0, 3);
    }

    // Generate abbreviation from the first three characters of taskName
    std::string::iterator it;
    for (it = abbreviation.begin(); it != abbreviation.end(); ++it) {
        *it = static_cast<char>(std::toupper(*it));
    }

    return abbreviation;
}

void TaskManager::updateAndLogTaskStates() {
    // Mutex should already be held when this is called
    std::ostringstream oss;
    std::vector<TaskInfo>::iterator it;
    
    for (it = taskList_.begin(); it != taskList_.end(); ++it) {
        if (it->taskHandle != nullptr) {
            // EXTRA SAFETY: Validate the handle value itself
            uintptr_t handleValue = (uintptr_t)(it->taskHandle);
            
            // Check for obviously corrupted handles
            if (handleValue < 0x3FFB0000 || handleValue > 0x3FFFFFFF) {
                // Handle is outside ESP32 RAM range
                oss << it->abbreviatedName << "=BAD,";
                it->lastKnownState = eDeleted;
                continue;
            }
            
            // CRITICAL FIX: Check if task is still valid before accessing it
            eTaskState taskState = eTaskGetState(it->taskHandle);
            
            // Check if task has been deleted
            if (taskState == eInvalid || taskState == eDeleted) {
                // Task has been deleted, mark it and skip
                oss << it->abbreviatedName << "=DEL,";
                it->lastKnownState = eDeleted;
                continue;
            }
            
            // Only access stack for tasks in stable states (not eReady)
            if (taskState == eRunning || taskState == eBlocked || taskState == eSuspended) {
                // Should be safe to get stack HWM
                UBaseType_t stackMark = uxTaskGetStackHighWaterMark(it->taskHandle);
                
                if (stackMark == 0 || stackMark > it->totalStackSize) {
                    // Stack mark is invalid
                    oss << it->abbreviatedName << "=inv,";
                    continue;
                }
                
                // Get priority
                UBaseType_t prio = uxTaskPriorityGet(it->taskHandle);
                
                // Calculate runtime and stack percentage
                TickType_t runTime = xTaskGetTickCount() - it->startTime;
                float stackPercentage = (stackMark * 100.0f) / it->totalStackSize;
                
                oss << it->abbreviatedName << "=" << taskStateToStringShort(taskState) << "("
                    << prio << ")"
                    << ",T:" << runTime << ",Stack:" << static_cast<int>(stackPercentage) << "%"
                    << (it->isPinned ? ",Core:P" : ",Core:NP");
                
                // Add watchdog status
                if (it->watchdogEnabled) {
                    oss << ",WDT:" << (it->watchdogCritical ? "C" : "N");
                }
                
                oss << ",";
            } else {
                // Task in eReady state - skip stack access entirely
                oss << it->abbreviatedName << "=" << taskStateToStringShort(taskState) << ",";
            }
            
            // Update last known state
            it->lastKnownState = taskState;
        } else {
            oss << it->abbreviatedName << "=nul,";
        }
    }
    
    std::string logStr = oss.str();
    if (!logStr.empty()) {
        logStr.pop_back();  // Remove trailing comma
    }
    TASKM_LOG_D( "%s", logStr.c_str());
}

// Alternative approach: Add a method to clean up deleted tasks
void TaskManager::cleanupDeletedTasks() {
    if (xSemaphoreTake(taskListMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    
    std::vector<TaskInfo>::iterator it = taskList_.begin();
    while (it != taskList_.end()) {
        if (it->taskHandle != nullptr) {
            eTaskState state = eTaskGetState(it->taskHandle);
            if (state == eDeleted || state == eInvalid) {
                TASKM_LOG_I( "Removing deleted task '%s' from list", 
                          it->taskName.c_str());
                it = taskList_.erase(it);
                continue;
            }
        }
        ++it;
    }
    
    xSemaphoreGive(taskListMutex);
}

// Hook for FreeRTOS task deletion (add to TaskManager class)
void TaskManager::handleTaskDeletion(TaskHandle_t taskHandle) {
    if (taskHandle == nullptr) return;
    
    if (xSemaphoreTake(taskListMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (auto& task : taskList_) {
            if (task.taskHandle == taskHandle) {
                TASKM_LOG_D( "Task '%s' is being deleted", 
                          task.taskName.c_str());
                task.lastKnownState = eDeleted;
                // We could remove it here or just mark it
                break;
            }
        }
        xSemaphoreGive(taskListMutex);
    }
}

// Static Task Creation Methods Implementation
/**
 * @brief Starts a new FreeRTOS task with static allocation.
 *
 * This method creates and starts a generic FreeRTOS task using static allocation. It's suitable for
 * tasks that do not require parameters and are not pinned to a specific core. This method is useful
 * in environments where dynamic memory allocation is discouraged or not feasible.
 *
 * @param taskFunction   Function pointer to the task function. The function should adhere to the
 * signature `void func(void*)`.
 * @param taskName       A descriptive name for the task, primarily used for debugging and logging
 * purposes.
 * @param stackBuffer    Pointer to the pre-allocated memory for the task's stack.
 * @param tcbBuffer      Pointer to the pre-allocated memory for the Task Control Block.
 * @param stackSize      The size of the stack in words (not bytes).
 * @param priority       The priority at which the task should run. Higher numbers represent higher
 * priorities.
 * @param abbreviatedName An optional abbreviated name for the task. If left empty, a default
 * abbreviation based on `taskName` is generated.
 *
 * @return true if the task is successfully created, false if the task creation fails.
 *
 * Usage Example:
 * @code
 *     TaskManager taskManager;
 *     StackType_t myStack[256];
 *     StaticTask_t myTCB;
 *     taskManager.startStaticTask(myTaskFunction, "MyTask", myStack, &myTCB, 256, 3, "MTK");
 * @endcode
 *
 * Note: The created task will be managed by the FreeRTOS scheduler and can run concurrently with
 * other tasks. Ensure that the stack size and priority are set appropriately for the intended task
 * functionality.
 */
bool TaskManager::startStaticTask(const StaticTaskConfig& config, const std::string& taskName,
                                  UBaseType_t priority, const std::string& abbreviatedName,
                                  const WatchdogConfig& watchdogConfig) {
    // Validate the stack and tcb buffers
    if (!config.stackBuffer || !config.tcbBuffer) {
        TASKM_LOG_E( "Invalid stack or TCB buffer for %s", taskName.c_str());
        return false;
    }

    if (config.stackSize > config.bufferCapacity) {
        TASKM_LOG_E( "Stack size exceeds buffer capacity for %s",
                   taskName.c_str());
        return false;
    }

    // Generate the FreeRTOS name (truncated)
    std::string freertosName = taskName.substr(0, configMAX_TASK_NAME_LEN - 1);

    // Create the task with static allocation
    TaskHandle_t taskHandle = xTaskCreateStatic(
        generalTaskWrapper,     // Wrapper function that adapts the function pointer for FreeRTOS
        freertosName.c_str(),   // Use truncated name
        config.stackSize,       // Size of the stack in words
        const_cast<TaskParams*>(&config.params),  // Cast the TaskParams object to a void pointer for the task
        priority,               // Priority of the task within the FreeRTOS scheduler
        config.stackBuffer,     // Pre-allocated stack buffer
        config.tcbBuffer        // Pre-allocated TCB buffer
    );

    // Check if the task was created successfully
    if (taskHandle == nullptr) {
        TASKM_LOG_E( "Failed to create static task %s", taskName.c_str());
        return false;
    }
    
    // Generate abbreviation and log tag
    std::string abbreviation = 
        abbreviatedName.empty() ? generateAbbreviatedName(taskName, "") : abbreviatedName;
    std::string logTag = abbreviation;  // Use abbreviation as log tag by default
    
    TASKM_LOG_I( "Static task %s created successfully", taskName.c_str());

    // Create TaskInfo with all required fields in correct order
    TaskInfo taskInfo = {
        taskHandle,             // Task handle
        taskName,               // Full task name
        freertosName,           // FreeRTOS name (truncated)
        abbreviation,           // Abbreviated name
        logTag,                 // Log tag
        xTaskGetTickCount(),    // Store the start time
        tskNO_AFFINITY,         // Default core ID (not pinned to a specific core)
        false,                  // Indicates that the task is not pinned
        config.stackSize,       // Initial stack size (in words)
        eReady,                 // Initial task state
        xTaskGetTickCount(),    // Initial state change timestamp
        // Watchdog fields
        false,                  // watchdogEnabled
        false,                  // watchdogCritical
        0,                      // watchdogFeedInterval
        0,                      // lastWatchdogFeed
        0,                      // watchdogMissedFeeds
        0                       // totalWatchdogFeeds
    };
    
    // Log task creation with all name information
    TASKM_LOG_I( 
               "Static task created - Full: %s, FreeRTOS: %s, Abbrev: %s, LogTag: %s",
               taskInfo.taskName.c_str(), 
               taskInfo.freertosName.c_str(),
               taskInfo.abbreviatedName.c_str(),
               taskInfo.logTag.c_str());
    
    // Add to task list using the existing method
    addTaskToList(taskInfo);

    // Configure watchdog if requested
    if (watchdogConfig.enableWatchdog && watchdogInitialized) {
        if (!configureTaskWatchdog(taskName, watchdogConfig)) {
            TASKM_LOG_W( "Failed to configure watchdog for task %s",
                       taskName.c_str());
        }
    }
    
    return true;
}

/**
 * @brief Starts a new task pinned to a specific CPU core with static allocation.
 *
 * This method is used to create and start a FreeRTOS task that is pinned (or locked) to a specific
 * core on the ESP32, using static allocation. Pinned tasks are guaranteed to run on the specified
 * core, which can be beneficial for performance reasons, especially in a multi-core environment
 * like the ESP32. This method is useful in environments where dynamic memory allocation is
 * discouraged or not feasible.
 *
 * @param taskFunction   Function pointer to the task function. This function should match the
 * signature `void func(void*)`.
 * @param taskName       A descriptive name for the task, used in debugging.
 * @param stackBuffer    Pointer to the pre-allocated memory for the task's stack.
 * @param tcbBuffer      Pointer to the pre-allocated memory for the Task Control Block.
 * @param stackSize      The size of the stack in words (not bytes).
 * @param parameter      A pointer to a variable to be passed to the task function. Can be `nullptr`
 * if no parameter is needed.
 * @param priority       The priority at which the task should run. Higher values indicate higher
 * priority.
 * @param coreID         The ID of the core to which the task should be pinned. Typically `0` or `1`
 * for ESP32.
 * @param abbreviatedName An optional short name for logging and tracking purposes. If left empty, a
 * default will be generated based on `taskName`.
 *
 * @return true if the task is successfully created and pinned to the specified core with static
 * allocation, false if the task creation fails.
 *
 * Usage Example:
 * @code
 *     TaskManager taskManager;
 *     StackType_t myStack[256];
 *     StaticTask_t myTCB;
 *     taskManager.startStaticTaskPinned(myTaskFunction, "MyPinnedTask", myStack, &myTCB, 256,
 * nullptr, 5, 1, "MPT");
 * @endcode
 *
 * Note: Pinning tasks to a core can help with real-time processing requirements but may lead to
 * underutilization of the other core. Use this feature judiciously to maintain system balance.
 */
bool TaskManager::startStaticTaskPinned(const StaticTaskConfig& config, const std::string& taskName,
                                        UBaseType_t priority, BaseType_t coreID,
                                        const std::string& abbreviatedName,
                                        const WatchdogConfig& watchdogConfig) {
    if (!config.stackBuffer || !config.tcbBuffer) {
        TASKM_LOG_E( "Invalid parameters for static pinned task creation");
        return false;
    }

    // Generate the FreeRTOS name (truncated)
    std::string freertosName = taskName.substr(0, configMAX_TASK_NAME_LEN - 1);

    TaskHandle_t taskHandle = xTaskCreateStaticPinnedToCore(
        generalTaskWrapper,     // Wrapper function that adapts the function pointer for FreeRTOS
        freertosName.c_str(),   // Use truncated name for FreeRTOS
        config.stackSize,       // Size of the stack in words
        static_cast<void*>(const_cast<TaskParams*>(&config.params)),  // Cast the TaskParams object
        priority,               // Priority of the task within the FreeRTOS scheduler
        config.stackBuffer,     // Pre-allocated stack buffer
        config.tcbBuffer,       // Pre-allocated TCB buffer
        coreID                  // Core ID where the task should run
    );

    // Check if the task was created successfully
    if (taskHandle == nullptr) {
        TASKM_LOG_E( "Failed to create static pinned task");
        return false;
    }

    // Task Creation Success Logic
    std::string abbreviation =
        abbreviatedName.empty() ? generateAbbreviatedName(taskName, "") : abbreviatedName;
    std::string logTag = abbreviation;  // Use abbreviation as log tag by default

    TaskInfo taskInfo = {
        taskHandle,             // Task handle
        taskName,               // Full task name
        freertosName,           // FreeRTOS name (truncated)
        abbreviation,           // Abbreviated name
        logTag,                 // Log tag
        xTaskGetTickCount(),    // Store the start time
        coreID,                 // Core ID for task affinity, indicating pinned core
        true,                   // Indicates that the task is pinned to a core
        config.stackSize,       // Initial stack size (in words)
        eReady,                 // Initial task state
        xTaskGetTickCount(),    // Initial state change timestamp
        // Watchdog fields
        false,  // watchdogEnabled
        false,  // watchdogCritical
        0,      // watchdogFeedInterval
        0,      // lastWatchdogFeed
        0,      // watchdogMissedFeeds
        0       // totalWatchdogFeeds
    };

    // Log task creation with all name information
    TASKM_LOG_I( 
               "Static pinned task created - Full: %s, FreeRTOS: %s, Abbrev: %s, Core: %d",
               taskInfo.taskName.c_str(), 
               taskInfo.freertosName.c_str(),
               taskInfo.abbreviatedName.c_str(),
               coreID);

    // Protect task list modification with a mutex and add the task info to the list
    addTaskToList(taskInfo);

    // Configure watchdog if requested
    if (watchdogConfig.enableWatchdog && watchdogInitialized) {
        if (!configureTaskWatchdog(taskName, watchdogConfig)) {
            TASKM_LOG_W( "Failed to configure watchdog for task %s",
                       taskName.c_str());
        }
    }

    return true;
}

/**
 * @brief Starts a task that executes a member function of the TaskManager class with static
 * allocation.
 *
 * This method allows for the creation of a FreeRTOS task that runs a specific member function of
 * the TaskManager class using static memory allocation. It encapsulates the complexities of
 * FreeRTOS task creation and management. The task will execute the specified member function until
 * it is explicitly deleted or the function completes.
 *
 * @param memberFunction Pointer to the member function of TaskManager to be executed by the task.
 *                       This member function must have a void return type and take no parameters.
 * @param taskName       A string representing the name of the task. This name is useful for
 * debugging purposes.
 * @param stackBuffer    Pointer to the pre-allocated memory for the task's stack.
 * @param tcbBuffer      Pointer to the pre-allocated memory for the Task Control Block.
 * @param stackSize      The size of the stack in words (not bytes).
 * @param priority       The priority at which the task should run. Tasks with higher priorities are
 * executed preferentially compared to lower priority tasks.
 * @param abbreviatedName An optional abbreviated name for the task, primarily used for logging.
 *                        If left empty, the first three characters of taskName will be used.
 *
 * @return true if the task was successfully created with static allocation and added to the task
 * list, false otherwise.
 *
 * Usage Example:
 * @code
 *     TaskManager taskManager;
 *     StackType_t myStack[256];
 *     StaticTask_t myTCB;
 *     taskManager.startStaticMemberFunctionTask(&TaskManager::someMemberFunction, "MyTask",
 * myStack, &myTCB, 256, 5, "MTK");
 * @endcode
 *
 * Note: This method utilizes static memory allocation for the task stack and TCB, which is
 * advantageous in environments where dynamic allocation is discouraged or not feasible.
 */
bool TaskManager::startStaticMemberFunctionTask(const StaticTaskConfig& config,
                                                const std::string& taskName, UBaseType_t priority,
                                                const std::string& abbreviatedName,
                                                const WatchdogConfig& watchdogConfig) {
    // Validate the stack and tcb buffers
    if (!config.stackBuffer || !config.tcbBuffer) {
        TASKM_LOG_E(
                   "Invalid parameters for static member function task creation");
        return false;
    }

    // Generate the FreeRTOS name (truncated)
    std::string freertosName = taskName.substr(0, configMAX_TASK_NAME_LEN - 1);

    // Create the task with static allocation
    TaskHandle_t taskHandle = xTaskCreateStatic(
        memberFunctionTaskWrapper,  // Wrapper function that adapts the member function for FreeRTOS
        freertosName.c_str(),       // Use truncated name for FreeRTOS
        config.stackSize,           // Size of the stack in words
        static_cast<void*>(const_cast<TaskParams*>(&config.params)),  // Pass the MemberFunctionWrapper object
        priority,                   // Priority of the task within the FreeRTOS scheduler
        config.stackBuffer,         // Pre-allocated stack buffer
        config.tcbBuffer            // Pre-allocated TCB buffer
    );

    // Error Handling
    if (taskHandle == nullptr) {
        TASKM_LOG_E( "Failed to create static member function task");
        return false;
    }

    // Successfully created the task, now store its information
    std::string abbreviation =
        abbreviatedName.empty() ? generateAbbreviatedName(taskName, "") : abbreviatedName;
    std::string logTag = abbreviation;  // Use abbreviation as log tag by default

    // Add the task info to the task list
    TaskInfo taskInfo = {
        taskHandle,             // Task handle
        taskName,               // Full task name
        freertosName,           // FreeRTOS name (truncated)
        abbreviation,           // Abbreviated name
        logTag,                 // Log tag
        xTaskGetTickCount(),    // Store the start time
        tskNO_AFFINITY,         // Default core ID (not pinned to a specific core)
        false,                  // Indicates that the task is not pinned
        config.stackSize,       // Initial stack size (in words)
        eReady,                 // Initial task state
        xTaskGetTickCount(),    // Initial state change timestamp
        // Watchdog fields
        false,  // watchdogEnabled
        false,  // watchdogCritical
        0,      // watchdogFeedInterval
        0,      // lastWatchdogFeed
        0,      // watchdogMissedFeeds
        0       // totalWatchdogFeeds
    };

    // Log task creation with all name information
    TASKM_LOG_I( 
               "Static member function task created - Full: %s, FreeRTOS: %s, Abbrev: %s, LogTag: %s",
               taskInfo.taskName.c_str(), 
               taskInfo.freertosName.c_str(),
               taskInfo.abbreviatedName.c_str(),
               taskInfo.logTag.c_str());

    // Protect task list modification with a mutex and add the task info to the list
    addTaskToList(taskInfo);

    // Configure watchdog if requested
    if (watchdogConfig.enableWatchdog && watchdogInitialized) {
        if (!configureTaskWatchdog(taskName, watchdogConfig)) {
            TASKM_LOG_W( "Failed to configure watchdog for task %s",
                       taskName.c_str());
        }
    }

    // The wrapper will automatically clean up when it goes out of scope
    return true;
}


std::string TaskManager::getFreeRTOSNameByFullName(const std::string& fullName) const {
    if (xSemaphoreTake(taskListMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return "";
    }
    
    std::string result;
    for (const auto& task : taskList_) {
        if (task.taskName == fullName) {
            result = task.freertosName;
            break;
        }
    }
    
    xSemaphoreGive(taskListMutex);
    return result;
}

std::string TaskManager::getLogTagByFullName(const std::string& fullName) const {
    if (xSemaphoreTake(taskListMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return "";
    }
    
    std::string result;
    for (const auto& task : taskList_) {
        if (task.taskName == fullName) {
            result = task.logTag;
            break;
        }
    }
    
    xSemaphoreGive(taskListMutex);
    return result;
}

std::vector<std::string> TaskManager::getAllLogTags() const {
    std::vector<std::string> tags;
    
    MutexGuard lock(taskListMutex, pdMS_TO_TICKS(100));
    if (!lock.hasLock()) {
        TASKM_LOG_W( "Failed to acquire mutex for getAllLogTags");
        return tags;
    }
    
    // Reserve space for efficiency
    tags.reserve(taskList_.size() * 2);
    
    for (const auto& task : taskList_) {
        tags.push_back(task.logTag);
        // Also add the FreeRTOS name as it might appear in logs
        if (task.freertosName != task.logTag) {
            tags.push_back(task.freertosName);
        }
    }
    
    return tags;
}

void TaskManager::configureTaskLogLevel(const std::string& taskFullName, esp_log_level_t level) {
    if (xSemaphoreTake(taskListMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    
    for (const auto& task : taskList_) {
        if (task.taskName == taskFullName) {
            // Note: LogInterface doesn't support per-tag log levels
            // This functionality requires the full Logger implementation
            
            TASKM_LOG_I( 
                       "Set log level %d for task %s (tags: %s, %s, %s)",
                       level, taskFullName.c_str(),
                       task.logTag.c_str(),
                       task.freertosName.c_str(), 
                       task.abbreviatedName.c_str());
            break;
        }
    }
    
    xSemaphoreGive(taskListMutex);
}

void TaskManager::configureTaskGroupLogLevel(const std::string& groupPrefix, esp_log_level_t level) {
    if (xSemaphoreTake(taskListMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    
    int count = 0;
    for (const auto& task : taskList_) {
        if (task.taskName.find(groupPrefix) != std::string::npos) {
            // Note: LogInterface doesn't support per-tag log levels
            // This functionality requires the full Logger implementation
            count++;
        }
    }
    
    TASKM_LOG_I( 
               "Set log level %d for %d tasks with prefix '%s'",
               level, count, groupPrefix.c_str());
    
    xSemaphoreGive(taskListMutex);
}



bool TaskManager::isTaskValid(TaskHandle_t handle) const noexcept {
    if (handle == nullptr) {
        return false;
    }
    
    eTaskState state = eTaskGetState(handle);
    return (state != eInvalid && state != eDeleted);
}

bool TaskManager::removeTaskFromList(TaskHandle_t handle) {
    if (handle == nullptr) {
        return false;
    }
    
    if (xSemaphoreTake(taskListMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    
    bool removed = false;
    std::vector<TaskInfo>::iterator it = taskList_.begin();
    while (it != taskList_.end()) {
        if (it->taskHandle == handle) {
            TASKM_LOG_I( "Removing task '%s' from list", 
                      it->taskName.c_str());
            it = taskList_.erase(it);
            removed = true;
            break;
        }
        ++it;
    }
    
    xSemaphoreGive(taskListMutex);
    return removed;
}

void TaskManager::getTaskCounts(int& validCount, int& deletedCount) const noexcept {
    validCount = 0;
    deletedCount = 0;
    
    if (xSemaphoreTake(taskListMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    
    for (const auto& task : taskList_) {
        if (task.taskHandle != nullptr) {
            eTaskState state = eTaskGetState(task.taskHandle);
            if (state == eInvalid || state == eDeleted) {
                deletedCount++;
            } else {
                validCount++;
            }
        } else {
            deletedCount++; // Null handle counts as deleted
        }
    }
    
    xSemaphoreGive(taskListMutex);
}

bool TaskManager::unregisterCurrentTaskFromWatchdog() {
    TaskHandle_t currentTask = xTaskGetCurrentTaskHandle();
    if (!currentTask) {
        return false;
    }
    
    // Remove from ESP-IDF watchdog
    esp_err_t err = esp_task_wdt_delete(currentTask);
    if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) {
        TASKM_LOG_E( "Failed to remove current task from watchdog: %d", err);
        return false;
    }
    
    // Update our internal tracking
    if (xSemaphoreTake(taskListMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (auto& task : taskList_) {
            if (task.taskHandle == currentTask) {
                // Clear ALL watchdog-related fields
                task.watchdogEnabled = false;
                task.watchdogCritical = false;
                task.watchdogFeedInterval = 0;
                task.lastWatchdogFeed = 0;
                task.watchdogMissedFeeds = 0;
                task.totalWatchdogFeeds = 0;
                
                TASKM_LOG_I( "Task %s unregistered from watchdog", 
                          task.taskName.c_str());
                break;
            }
        }
        xSemaphoreGive(taskListMutex);
    }
    
    return true;
}

// Add to TaskManager.cpp:
TaskManager::TaskStatistics TaskManager::getTaskStatistics() const {
    TaskStatistics stats = {0};
    
    if (xSemaphoreTake(taskListMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return stats;
    }
    
    for (const auto& task : taskList_) {
        stats.totalTasks++;
        
        if (task.taskHandle == nullptr) {
            stats.invalidHandles++;
            continue;
        }
        
        // Check handle validity
        uintptr_t handleValue = (uintptr_t)(task.taskHandle);
        if (handleValue < 0x3FFB0000 || handleValue > 0x3FFFFFFF) {
            stats.invalidHandles++;
            continue;
        }
        
        eTaskState state = eTaskGetState(task.taskHandle);
        switch (state) {
            case eRunning:
                stats.runningTasks++;
                if (task.watchdogEnabled) stats.watchdogTasks++;
                break;
            case eReady:
                stats.readyTasks++;
                if (task.watchdogEnabled) stats.watchdogTasks++;
                break;
            case eBlocked:
                stats.blockedTasks++;
                if (task.watchdogEnabled) stats.watchdogTasks++;
                break;
            case eSuspended:
                stats.suspendedTasks++;
                if (task.watchdogEnabled) stats.watchdogTasks++;
                break;
            case eDeleted:
            case eInvalid:
                stats.deletedTasks++;
                break;
        }
    }
    
    xSemaphoreGive(taskListMutex);
    return stats;
}

// Configurable watchdog miss threshold - IMPLEMENTED
// See WatchdogConfig in TaskManager.h for:
// - missThreshold: consecutive misses before action (default: 5 normal, 3 critical)
// - missAction: WatchdogAction enum (WARN_ONLY, SUSPEND_TASK, RESTART_TASK, SYSTEM_RESET)
// - TaskInfo now tracks watchdogThresholdExceeded flag
//
// Example usage:
//    auto config = WatchdogConfig::enabled(true, 5000)
//                      .withThreshold(3)
//                      .withAction(WatchdogAction::RESTART_TASK);

bool TaskManager::stopTask(TaskHandle_t handle) {
    if (!handle) {
        TASKM_LOG_E( "stopTask: Invalid task handle");
        return false;
    }
    
    RecursiveMutexGuard guard(taskListMutex, pdMS_TO_TICKS(5000));
    if (!guard.hasLock()) {
        TASKM_LOG_E( "stopTask: Failed to acquire mutex");
        return false;
    }
    
    // Find the task in our list
    auto it = std::find_if(taskList_.begin(), taskList_.end(),
        [handle](const TaskInfo& task) { return task.taskHandle == handle; });
    
    if (it != taskList_.end()) {
        // Store name for logging
        std::string taskName = it->taskName;
        
        // Unregister from watchdog if enabled
        if (it->watchdogEnabled && watchdogInitialized) {
            Watchdog::getInstance().unregisterTaskByHandle(handle, it->taskName.c_str());
        }
        
        // Remove from our tracking list first
        taskList_.erase(it);
        
        // Delete the FreeRTOS task
        vTaskDelete(handle);
        
        TASKM_LOG_I( "Task %s stopped successfully", taskName.c_str());
        return true;
    }
    
    TASKM_LOG_W( "stopTask: Task handle not found in task list");
    return false;
}
// - Configurable per-task based on criticality
// - Better suited for production environments with varying load conditions