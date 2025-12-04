// TaskManager_Full.h - Original full-featured implementation
#ifndef TASKMANAGER_FULL_H
#define TASKMANAGER_FULL_H

#include <string>
#include <vector>
#include <algorithm>  // for std::transform
#include <cctype>     // for ::toupper

#include "MutexGuard.h"
#include "RecursiveMutexGuard.h"
#include "Watchdog.h"
#include "cstring"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// Use centralized logging configuration
#include "TaskManagerLogging.h"

/**
 * @brief Structure to hold parameters for a task.
 *
 * This structure is used to pass a function pointer and its argument to a task in FreeRTOS.
 */
struct TaskParams {
    TaskParams(void (*f)(void*), void* a) : function(f), arg(a) {}
    void (*function)(void*);
    void* arg;
};

struct StaticTaskConfig {
    TaskParams params;
    StackType_t* stackBuffer;
    StaticTask_t* tcbBuffer;
    uint16_t stackSize;       // In words
    uint16_t bufferCapacity;  // New field for actual buffer size
};

/**
 * @class TaskManager
 * @brief Manages the creation and monitoring of FreeRTOS tasks with integrated watchdog support.
 *
 * TaskManager provides functionality to create, monitor, and manage tasks in a FreeRTOS
 * environment. It supports creating tasks with or without parameters, pinned or unpinned to
 * specific cores. Additionally, it offers extensive debugging capabilities to monitor task states
 * and system resource usage. Now includes comprehensive watchdog timer integration for improved
 * system reliability.
 */
struct TaskStateMessage {
    static const size_t MAX_TASK_NAME = 32;
    char taskName[MAX_TASK_NAME];
    eTaskState state;
    TickType_t timestamp;

    TaskStateMessage() {
        memset(taskName, 0, MAX_TASK_NAME);
        state = eInvalid;
        timestamp = 0;
    }
};

struct TaskStateChange {
    static const size_t MAX_TASK_NAME = 32;
    char taskName[MAX_TASK_NAME];
    eTaskState newState;

    TaskStateChange() {
        memset(taskName, 0, MAX_TASK_NAME);
        newState = eInvalid;
    }

    TaskStateChange(const char* name, eTaskState state) {
        strncpy(taskName, name, MAX_TASK_NAME - 1);
        taskName[MAX_TASK_NAME - 1] = '\0';
        newState = state;
    }
};


class TaskManager {
   public:
    /**
     * @struct WatchdogConfig
     * @brief Configuration for task watchdog behavior
     *
     * This structure defines how a task should interact with the watchdog timer.
     */
    struct WatchdogConfig {
        bool enableWatchdog;      // Whether to add task to watchdog
        uint32_t feedIntervalMs;  // Expected feed interval (0 = use global default)
        bool criticalTask;        // If true, missing feeds will trigger system reset

        WatchdogConfig() : enableWatchdog(false), feedIntervalMs(0), criticalTask(false) {}

        // Convenience constructors
        static WatchdogConfig disabled() { return WatchdogConfig(); }
        static WatchdogConfig enabled(bool critical = false, uint32_t intervalMs = 0) {
            WatchdogConfig cfg;
            cfg.enableWatchdog = true;
            cfg.criticalTask = critical;
            cfg.feedIntervalMs = intervalMs;
            return cfg;
        }
    };

    /**
     * @struct TaskInfo
     * @brief Holds information about a task managed by TaskManager, including watchdog status.
     *
     * This structure stores details of a task such as its handle, name, start time, stack size,
     * and watchdog monitoring information.
     */
    struct TaskInfo {
        TaskHandle_t taskHandle;
        std::string taskName;           // Full descriptive name (e.g., "ModbusStatusTask")
        std::string freertosName;       // Actual FreeRTOS name (max 16 chars, e.g., "ModbusStatusTas")
        std::string abbreviatedName;    // Short abbreviation for logging (e.g., "MbStat")
        std::string logTag;             // Tag used for logging (could be same as abbreviatedName or different)
        TickType_t startTime;     // To store the start time of the task
        BaseType_t coreID;        // To store the core ID where the task is running/pinned
        bool isPinned;            // Flag to indicate if the task is pinned to a core
        uint16_t totalStackSize;  // To store the total stack size of the task
        eTaskState lastKnownState;
        TickType_t lastStateChangeTick;

        // Watchdog fields
        bool watchdogEnabled;
        bool watchdogCritical;
        uint32_t watchdogFeedInterval;
        TickType_t lastWatchdogFeed;
        uint32_t watchdogMissedFeeds;
        uint32_t totalWatchdogFeeds;
    };

    /**
     * @struct TaskStatistics
     * @brief Provides detailed statistics about tasks in the system
     */
    struct TaskStatistics {
        uint32_t totalTasks;
        uint32_t runningTasks;
        uint32_t blockedTasks;
        uint32_t readyTasks;
        uint32_t suspendedTasks;
        uint32_t deletedTasks;
        uint32_t invalidHandles;
        uint32_t watchdogTasks;
    };

    // Structure to hold all name variants for a task
    struct TaskNames {
        std::string fullName;        // Full descriptive name
        std::string freertosName;    // Name to give to FreeRTOS (max 16 chars)
        std::string abbreviation;    // Short form for display
        std::string logTag;          // Tag for logging
        
        // Constructor with smart defaults
        TaskNames(const std::string& full, 
                  const std::string& abbrev = "", 
                  const std::string& tag = "") 
            : fullName(full) {
            
            // Generate FreeRTOS name (truncate to 15 chars + null terminator)
            freertosName = full.substr(0, configMAX_TASK_NAME_LEN - 1);
            
            // Generate abbreviation if not provided
            abbreviation = abbrev.empty() ? generateSmartAbbreviation(full) : abbrev;
            
            // Use abbreviation as log tag if not provided
            logTag = tag.empty() ? abbreviation : tag;
        }
        
    private:
        static std::string generateSmartAbbreviation(const std::string& fullName) {
            // Smart abbreviation based on common patterns
            if (fullName.find("Modbus") != std::string::npos) {
                if (fullName.find("Status") != std::string::npos) return "MbStat";
                if (fullName.find("Control") != std::string::npos) return "MbCtrl";
                return "Mb";
            }
            if (fullName.find("Monitoring") != std::string::npos) return "Mon";
            if (fullName.find("OTA") != std::string::npos) return "OTA";
            if (fullName.find("Debug") != std::string::npos) return "Dbg";
            
            // Default: first 3 chars uppercase
            std::string abbrev = fullName.substr(0, 3);
            std::transform(abbrev.begin(), abbrev.end(), abbrev.begin(), ::toupper);
            return abbrev;
        }
    };
    
    // Enhanced startTask method
    bool startTask(void (*taskFunction)(void*), 
                   const TaskNames& names,
                   uint16_t stackSize, 
                   void* parameter, 
                   UBaseType_t priority,
                   const WatchdogConfig& watchdogConfig = WatchdogConfig::disabled());
    
    // Convenience overload that maintains backward compatibility
    bool startTask(void (*taskFunction)(void*), 
                   const std::string& taskName,
                   uint16_t stackSize, 
                   void* parameter, 
                   UBaseType_t priority,
                   const std::string& abbreviatedName = "",
                   TaskHandle_t* outTaskHandle = nullptr,
                   const WatchdogConfig& watchdogConfig = WatchdogConfig::disabled()) {
        
        TaskNames names(taskName, abbreviatedName);
        bool result = startTask(taskFunction, names, stackSize, parameter, priority, watchdogConfig);
        if (result && outTaskHandle) {
            *outTaskHandle = getTaskHandleByName(taskName);
        }
        return result;
    }
    
    // Methods to get various name representations
    std::string getFreeRTOSNameByFullName(const std::string& fullName) const;
    std::string getLogTagByFullName(const std::string& fullName) const;
    std::vector<std::string> getAllLogTags() const;
    
    // Method to configure log levels for all variants of a task's names
    void configureTaskLogLevel(const std::string& taskFullName, esp_log_level_t level);
    
    // Bulk configuration for related tasks
    void configureTaskGroupLogLevel(const std::string& groupPrefix, esp_log_level_t level);

    // Structure for wrapping member function calls for FreeRTOS tasks
    /**
     * @struct MemberFunctionWrapperData
     * @brief Encapsulates data for wrapping a member function call.
     *
     * This structure is used to wrap a member function of TaskManager so that it can be called as a
     * FreeRTOS task.
     */
    struct MemberFunctionWrapperData {
        TaskManager* instance;
        void (TaskManager::*memberFunction)(void);
    };

    // Constructor and Destructor
    /**
     * @brief Constructor for TaskManager.
     */
    TaskManager();

    /**
     * @brief Destructor for TaskManager.
     *
     * Cleans up any resources or tasks managed by this instance.
     */
    ~TaskManager();

    // Delete copy constructor and assignment operator
    TaskManager(const TaskManager&) = delete;
    TaskManager& operator=(const TaskManager&) = delete;

    // Move constructor and assignment operator declarations (implemented in cpp)
    TaskManager(TaskManager&& other);
    TaskManager& operator=(TaskManager&& other);

    // ============== Watchdog Management Methods ==============

    /**
     * @brief Initialize or reconfigure the ESP32 task watchdog timer
     *
     * @param timeoutSeconds Watchdog timeout in seconds (default: 30)
     * @param panicOnTimeout Whether to trigger panic/reset on timeout (default: true)
     * @return true if initialization successful
     */
    bool initWatchdog(uint32_t timeoutSeconds = 30, bool panicOnTimeout = true);

    /**
     * @brief Check if watchdog is initialized
     * @return true if watchdog is initialized
     */
    bool isWatchdogInitialized() const { return watchdogInitialized; }

    /**
     * @brief Feed the watchdog for the current task
     *
     * Tasks should call this regularly to prevent watchdog timeout.
     * This is a static method so it can be called from any task function.
     *
     * @return true if watchdog was successfully fed
     */
    bool feedWatchdog();

    /**
     * @brief Enable/disable watchdog for a specific task
     *
     * @param taskName Name of the task
     * @param config Watchdog configuration
     * @return true if successful
     */
    bool configureTaskWatchdog(const std::string& taskName, const WatchdogConfig& config);

    bool registerCurrentTaskWithWatchdog(const std::string& taskName, const WatchdogConfig& config);
    
    /**
     * @brief Unregister the current task from watchdog monitoring
     * 
     * This method should be called before a task exits if it was previously
     * registered with the watchdog. It removes the task from watchdog monitoring
     * to prevent false timeouts after the task is deleted.
     * 
     * @return true if successfully unregistered, false on error
     */
    bool unregisterCurrentTaskFromWatchdog();
    
    void registerTask(const std::string& taskName, TaskHandle_t handle);

    /**
     * @brief Get watchdog statistics for a specific task
     *
     * @param taskName Name of the task
     * @param[out] missedFeeds Number of missed feeds
     * @param[out] totalFeeds Total number of successful feeds
     * @return true if task found and has watchdog enabled
     */
    bool getTaskWatchdogStats(const std::string& taskName, uint32_t& missedFeeds,
                              uint32_t& totalFeeds) const;

    /**
     * @brief Log watchdog statistics for all tasks
     */
    void logWatchdogStats() const;

    /**
     * @brief Set global watchdog feed check interval
     *
     * @param intervalMs Interval in milliseconds for checking task watchdog status
     */
    void setWatchdogCheckInterval(uint32_t intervalMs);

    /**
     * @brief Get detailed task statistics
     * @return TaskStatistics structure with current task counts
     */
    TaskStatistics getTaskStatistics() const;

    // ============== Task Management Methods ==============

    /**
     * @brief Starts a FreeRTOS task using a member function.
     *
     * Creates and starts a FreeRTOS task that executes a member function of the TaskManager class.
     *
     * @param memberFunction Pointer to the member function to execute.
     * @param taskName The name of the task.
     * @param stackSize The stack size for the task.
     * @param priority The priority at which the task should run.
     * @param abbreviatedName An optional abbreviated name for the task.
     * @param outTaskHandle Optional pointer to store the created task's handle.
     * @param watchdogConfig Optional watchdog configuration (default: disabled)
     * @return true if the task was successfully created, false otherwise.
     */
    bool startTask(void (TaskManager::*memberFunction)(void), const std::string& taskName,
                   uint16_t stackSize, UBaseType_t priority,
                   const std::string& abbreviatedName = "", TaskHandle_t* outTaskHandle = nullptr,
                   const WatchdogConfig& watchdogConfig = WatchdogConfig::disabled());

    /**
     * @brief Starts a FreeRTOS task pinned to a specific core.
     *
     * Creates and starts a FreeRTOS task, pinning it to a specific CPU core. Optionally retrieves
     * the task handle.
     *
     * @param taskFunction Function pointer to the task function.
     * @param taskName The name of the task.
     * @param stackSize The stack size for the task.
     * @param parameter The parameter to pass to the task function.
     * @param priority The priority at which the task should run.
     * @param coreID The core to which the task is pinned. Use `tskNO_AFFINITY` to allow the
     * scheduler to decide.
     * @param abbreviatedName An optional abbreviated name for the task.
     * @param outTaskHandle Optional pointer to store the created task's handle.
     * @param watchdogConfig Optional watchdog configuration (default: disabled)
     * @return true if the task was successfully created, false otherwise.
     */
    bool startTaskPinned(void (*taskFunction)(void*), const std::string& taskName,
                         uint16_t stackSize, void* parameter, UBaseType_t priority,
                         BaseType_t coreID, const std::string& abbreviatedName = "",
                         TaskHandle_t* outTaskHandle = nullptr,
                         const WatchdogConfig& watchdogConfig = WatchdogConfig::disabled());

    /**
     * @brief Starts a new FreeRTOS task using static allocation.
     *
     * Creates and starts a FreeRTOS task with static memory allocation. This method is suitable for
     * environments where dynamic memory allocation is discouraged or not feasible.
     *
     * @param config          Configuration containing all static resources and task parameters.
     * @param taskName         A descriptive name for the task.
     * @param priority         The priority at which the task should run.
     * @param abbreviatedName  An optional abbreviated name for the task.
     * @param watchdogConfig   Optional watchdog configuration (default: disabled)
     *
     * @return true if the task is successfully created, false if creation fails.
     */
    bool startStaticTask(const StaticTaskConfig& config, const std::string& taskName,
                         UBaseType_t priority, const std::string& abbreviatedName = "",
                         const WatchdogConfig& watchdogConfig = WatchdogConfig::disabled());

    /**
     * @brief Starts a task pinned to a specific core using static allocation.
     *
     * @param config          Static task configuration.
     * @param taskName        Task name for debugging.
     * @param priority        Task priority.
     * @param coreID         Core to pin the task to.
     * @param abbreviatedName Optional abbreviated name.
     * @param watchdogConfig  Optional watchdog configuration (default: disabled)
     */
    bool startStaticTaskPinned(const StaticTaskConfig& config, const std::string& taskName,
                               UBaseType_t priority, BaseType_t coreID,
                               const std::string& abbreviatedName = "",
                               const WatchdogConfig& watchdogConfig = WatchdogConfig::disabled());

    /**
     * @brief Starts a member function task using static allocation.
     *
     * @param config          Static task configuration including member function.
     * @param taskName        Task name for debugging.
     * @param priority        Task priority.
     * @param abbreviatedName Optional abbreviated name.
     * @param watchdogConfig  Optional watchdog configuration (default: disabled)
     */
    bool startStaticMemberFunctionTask(
        const StaticTaskConfig& config, const std::string& taskName, UBaseType_t priority,
        const std::string& abbreviatedName = "",
        const WatchdogConfig& watchdogConfig = WatchdogConfig::disabled());

    // Task Management Methods
    /**
     * @brief Gets a list of all tasks managed by this instance.
     *
     * @return A constant reference to the vector containing task information.
     */
    const std::vector<TaskInfo>& getTaskList() const noexcept;

    /**
     * @brief Sets the interval for debug task execution.
     *
     * This interval determines how often the debug task loops and processes state changes.
     *
     * @param interval_ms The interval in milliseconds.
     */
    void setTaskExecutionInterval(uint32_t interval_ms);

    /**
     * @brief Sets the interval for periodic resource usage and task state logging.
     *
     * This interval determines how often resource usage and task state information
     * are logged by the debug task.
     *
     * @param interval_ms The interval in milliseconds.
     */
    void setResourceLogPeriod(uint32_t interval_ms);

    /**
     * @brief Debug task that logs task and system information periodically.
     *
     * This method is intended to run as a task in FreeRTOS. It periodically logs information about
     * all tasks managed by the TaskManager and system resource usage.
     */
    void debugTask();

    void monitorTaskNotifications();

    // Method to get the handle of a task by its name
    TaskHandle_t getTaskHandleByName(const std::string& taskName) const noexcept;
    
    // Task control
    bool stopTask(TaskHandle_t handle);

    /**
     * @brief Retrieves the name of a task from its handle.
     *
     * This method provides a convenient way to get the name of a task using its handle.
     * It's particularly useful when handling task notifications or state changes where
     * the task handle is available, but the name is needed for logging or debugging.
     *
     * @param taskHandle The handle of the task whose name is to be retrieved.
     * @return A string containing the name of the task, or "Invalid Task" if the handle is null.
     */
    std::string taskNameFromHandle(TaskHandle_t taskHandle) const;
    bool notifyTaskStateChange(const TaskStateChange& stateChange);
    
    /**
     * @brief Remove deleted tasks from the task list
     * 
     * This method iterates through the task list and removes any tasks
     * that have been deleted (eTaskGetState returns eDeleted or eInvalid).
     * Should be called periodically to keep the task list clean.
     */
    void cleanupDeletedTasks();
    
    /**
     * @brief Check if a task handle is still valid
     * 
     * @param handle The task handle to check
     * @return true if the task is still valid and running, false if deleted or invalid
     */
    bool isTaskValid(TaskHandle_t handle) const noexcept;
    
    /**
     * @brief Remove a specific task from the list by handle
     * 
     * @param handle The task handle to remove
     * @return true if the task was found and removed, false otherwise
     */
    bool removeTaskFromList(TaskHandle_t handle);
    
    /**
     * @brief Get count of valid vs deleted tasks
     * 
     * @param[out] validCount Number of tasks that are still valid
     * @param[out] deletedCount Number of tasks that have been deleted but still in list
     */
    void getTaskCounts(int& validCount, int& deletedCount) const noexcept;

    // Static Methods
    /**
     * @brief Wrapper function for the debug task to adapt to FreeRTOS task signature.
     *
     * @param pvParameters Parameters passed to the task, expected to be a pointer to TaskManager
     * instance.
     */
    static void debugTaskWrapper(void* pvParameters);

    /**
     * @brief General wrapper function for FreeRTOS tasks.
     *
     * This function is used to adapt a regular function to be suitable for FreeRTOS task creation.
     *
     * @param pvParameters Parameters for the task function.
     */
    static void generalTaskWrapper(void* pvParameters);

    /**
     * @brief Converts a FreeRTOS task state to a readable string.
     *
     * @param taskState The state of the task in the FreeRTOS eTaskState format.
     * @return A string representing the task state.
     */
    static std::string taskStateToString(eTaskState taskState);

    /**
     * @brief Converts a FreeRTOS task state to a short, readable string.
     *
     * This version provides a concise string representation, suitable for logging or display in
     * constrained spaces.
     *
     * @param taskState The state of the task in the FreeRTOS eTaskState format.
     * @return A short string representing the task state.
     */
    static const char* taskStateToStringShort(eTaskState taskState);

    /**
     * @brief Converts a FreeRTOS scheduler state to a readable string.
     *
     * @param state The state of the scheduler (running, suspended, or not started).
     * @return A string representing the scheduler state.
     */
    static const char* schedulerStateToString(int state);

    // Public Member Variables
    TaskHandle_t debugTaskHandle;

   private:
    /**
     * @brief Static constant tag used for logging purposes.
     *
     * This tag is used to identify logs coming from the TaskManager.
     */
    static const char* tag;

    /**
     * @var std::vector<TaskInfo> taskList_
     * @brief A list of all tasks managed by the TaskManager.
     *
     * Stores detailed information about each task, including its handle, name, start time, and
     * stack size.
     */
    std::vector<TaskInfo> taskList_;

    /**
     * @var TickType_t debugInterval
     * @brief The interval at which the debug task logs information.
     *
     * Defaults to 30 seconds. Determines how frequently the debug task logs task and system
     * information.
     */
    // uint32_t taskExecutionInterval = pdMS_TO_TICKS(200); // Default interval, adjustable
    TickType_t taskExecutionInterval = pdMS_TO_TICKS(100);  // Default 100ms

    /**
     * @brief Set the periode for resource usage and task state logging.
     * @param interval_ms Interval in milliseconds.
     */
    TickType_t resourceLogPeriod = pdMS_TO_TICKS(20000);  // Default 20s

    /**
     * @var SemaphoreHandle_t taskListMutex
     * @brief A mutex for thread-safe access to the taskList_.
     *
     * Ensures that access to the taskList_ vector is synchronized across different tasks.
     */
    SemaphoreHandle_t taskListMutex;  // Mutex for thread-safe access to taskList_

    // Queue handle
    QueueHandle_t taskStateQueue;

    // Watchdog related members
    // Watchdog instance removed - using singleton pattern from Watchdog 2.0.0
    bool watchdogInitialized;
    uint32_t watchdogTimeoutSeconds;
    TaskHandle_t watchdogMonitorTaskHandle;
    TickType_t watchdogCheckInterval;

    // Private Methods
    /**
     * @brief Wrapper function for member function tasks.
     *
     * Used internally to adapt a member function of TaskManager for use as a FreeRTOS task.
     *
     * @param pvParameters Pointer to MemberFunctionWrapperData containing the instance and member
     * function to call.
     */
    static void memberFunctionTaskWrapper(void* pvParameters);

    /**
     * @brief Handles task deletion notification
     * 
     * Called before a task is deleted to update its status in the task list.
     * This helps prevent accessing deleted tasks.
     * 
     * @param taskHandle Handle of the task being deleted
     */
    void handleTaskDeletion(TaskHandle_t taskHandle);

    /**
     * @brief Initializes the mutex used for synchronizing access to the task list.
     *
     * This method creates a mutex to ensure thread-safe operations on the task list. It should be
     * called during the construction of the TaskManager object to ensure that the mutex is
     * available for use whenever task list operations are performed.
     *
     * Usage Example:
     * @code
     *     TaskManager taskManager;
     *     taskManager.initMutex();
     * @endcode
     *
     * Note: If the mutex cannot be created, an error should be logged or handled appropriately.
     */
    void initMutex();

    /**
     * @brief Adds a task to the managed task list in a thread-safe manner.
     *
     * This method adds a new task to the task list. It ensures thread safety by acquiring a mutex
     * before modifying the list and releasing it afterwards. This method should be used to add
     * tasks to the task list instead of directly pushing to the list, to prevent race conditions.
     *
     * @param taskInfo A reference to the TaskInfo structure containing information about the task.
     *
     * Usage Example:
     * @code
     *     TaskManager taskManager;
     *     TaskManager::TaskInfo taskInfo = {taskHandle, "MyTask", "MTK", xTaskGetTickCount(), 0,
     * false, 1024}; taskManager.addTaskToList(taskInfo);
     * @endcode
     *
     * Note: This method assumes that the taskListMutex has been properly initialized.
     */
    void addTaskToList(const TaskInfo& taskInfo);

    /**
     * @brief Updates the state of a task and logs any state changes.
     *
     * This method checks the current state of a task against its last known state. If the state has
     * changed, it updates the task's state information and logs the change. This is useful for
     * debugging and tracking the behavior of tasks over time.
     *
     * @param task A reference to the TaskInfo structure of the task to be updated.
     *
     * Usage Example:
     * @code
     *     TaskManager taskManager;
     *     // ... after tasks have been created and added to the list ...
     *     for (auto& task : taskManager.getTaskList()) {
     *         taskManager.updateTaskState(task);
     *     }
     * @endcode
     *
     * Note: This method is intended to be called within a loop that iterates over all managed
     * tasks, typically in a debug task or similar monitoring function.
     */
    void updateTaskState(TaskInfo& task);
    void handleTaskStateChange(const std::string& taskName, eTaskState newState);
    void updateAndLogTaskStates();

    /**
     * @brief Logs detailed information about a specific task.
     *
     * @param task The TaskInfo structure containing information about the task.
     *
     * Logs various details such as the task's state, priority, runtime, stack usage, and core
     * affinity.
     */
    void logTaskInfo(const TaskInfo& task);

    /**
     * @brief Prints extensive resource usage information for the system and tasks.
     *
     * This method logs detailed information about system resources like heap memory and PSRAM, as
     * well as individual task resource usage.
     */
    void printExtensiveResourceUsage();

    /**
     * @brief Generates an abbreviated name for a task.
     *
     * Creates a shortened version of a task's name for identification purposes.
     *
     * @param taskName The full name of the task.
     * @param customAbbreviation Optional custom abbreviation. If empty, the first three characters
     * of taskName are used.
     * @return A string representing the abbreviated name.
     */
    std::string generateAbbreviatedName(const std::string& taskName,
                                        const std::string& customAbbreviation);

    // Watchdog private methods
    /**
     * @brief Configure watchdog for a task (internal method)
     */
    bool configureTaskWatchdogInternal(TaskInfo& task, const WatchdogConfig& config);

    /**
     * @brief Monitor task that checks watchdog status
     */
    void watchdogMonitorTask();

    /**
     * @brief Check watchdog status for all tasks
     */
    void checkTaskWatchdogStatus();

    /**
     * @brief Handle critical watchdog failure
     */
    void handleCriticalWatchdogFailure(const TaskInfo& task);
};

#endif  // TASKMANAGER_FULL_H