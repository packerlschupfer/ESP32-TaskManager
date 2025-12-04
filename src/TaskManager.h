// TaskManager.h - Optimized version with smaller footprint (default)
#ifndef TASKMANAGER_H
#define TASKMANAGER_H

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <vector>
#include <cstring>
#include <atomic>
#include "MutexGuard.h"
#include "RecursiveMutexGuard.h"
#include "TaskManagerSafety.h"
#include "TaskRecovery.h"
#include "ResourceLeakDetector.h"
#include "IWatchdog.h"

// Use centralized logging configuration
#include "TaskManagerLogging.h"

// Configuration flags to enable/disable features at compile time
#define TM_ENABLE_WATCHDOG      1  // Enable watchdog support
#define TM_ENABLE_DEBUG_TASK    1  // Enable debug task
#define TM_ENABLE_TASK_NAMES    1  // Enable full task name tracking
#define TM_ENABLE_STATE_TRACKING 0 // Disable state change tracking
#define TM_ENABLE_STATIC_TASKS  0  // Disable static task support
#define TM_ENABLE_MEMBER_TASKS  0  // Disable member function tasks

// Optimize string storage
#define TM_MAX_TASK_NAME_LEN    16  // Reduced from 32
#define TM_MAX_ABBREV_LEN       4   // Short abbreviations

/**
 * @brief Structure to hold parameters for a task.
 */
struct TaskParams {
    TaskParams(void (*f)(void*), void* a) : function(f), arg(a) {}
    void (*function)(void*);
    void* arg;
};

/**
 * @class TaskManager
 * @brief Optimized task manager with reduced memory footprint
 */
class TaskManager {
public:
    /**
     * @enum WatchdogAction
     * @brief Action to take when watchdog miss threshold is exceeded
     */
    enum class WatchdogAction : uint8_t {
        WARN_ONLY,      ///< Log warnings only
        SUSPEND_TASK,   ///< Suspend the offending task
        RESTART_TASK,   ///< Attempt to restart the task
        SYSTEM_RESET    ///< Reset the system (for critical tasks)
    };

    /**
     * @struct WatchdogConfig
     * @brief Simplified watchdog configuration with threshold support
     */
    struct WatchdogConfig {
        bool enableWatchdog;
        uint32_t feedIntervalMs;
        bool criticalTask;
        uint8_t missThreshold;        ///< Consecutive misses before action (default: 5 normal, 3 critical)
        WatchdogAction missAction;    ///< Action when threshold exceeded

        WatchdogConfig()
            : enableWatchdog(false)
            , feedIntervalMs(0)
            , criticalTask(false)
            , missThreshold(5)
            , missAction(WatchdogAction::WARN_ONLY) {}

        static WatchdogConfig disabled() { return WatchdogConfig(); }
        static WatchdogConfig enabled(bool critical = false, uint32_t intervalMs = 0) {
            WatchdogConfig cfg;
            cfg.enableWatchdog = true;
            cfg.criticalTask = critical;
            cfg.feedIntervalMs = intervalMs;
            cfg.missThreshold = critical ? 3 : 5;  // Lower threshold for critical tasks
            cfg.missAction = critical ? WatchdogAction::SYSTEM_RESET : WatchdogAction::WARN_ONLY;
            return cfg;
        }

        WatchdogConfig& withThreshold(uint8_t threshold) { missThreshold = threshold; return *this; }
        WatchdogConfig& withAction(WatchdogAction action) { missAction = action; return *this; }
    };

    /**
     * @struct TaskInfo
     * @brief Optimized task information structure with safety enhancements
     */
    struct TaskInfo {
        TaskHandle_t taskHandle;
        char taskName[TM_MAX_TASK_NAME_LEN];      // Fixed size instead of std::string
        #if TM_ENABLE_TASK_NAMES
        char abbreviatedName[TM_MAX_ABBREV_LEN];  // Fixed size
        #endif
        TickType_t startTime;
        uint16_t totalStackSize;
        
        #if TM_ENABLE_WATCHDOG
        uint16_t watchdogFeedInterval;  // Reduced from uint32_t
        std::atomic<uint16_t> watchdogMissedFeeds;   // Atomic for thread safety
        std::atomic<TickType_t> lastWatchdogFeed;    // Track last feed time
        uint8_t watchdogMissThreshold;               // Consecutive misses before action
        WatchdogAction watchdogMissAction;           // Action when threshold exceeded
        uint8_t watchdogEnabled : 1;
        uint8_t watchdogCritical : 1;
        uint8_t watchdogThresholdExceeded : 1;       // Flag when threshold exceeded
        #endif
        
        uint8_t isPinned : 1;
        uint8_t coreID : 2;  // 0-3 cores max
        uint8_t hasStackWarning : 1;  // Stack usage warning
        uint8_t hasCrashed : 1;       // Task crash detection
        uint8_t reserved : 1;
        
        // Stack monitoring
        std::atomic<uint32_t> stackHighWaterMark;
        
        // Default constructor
        TaskInfo() : 
            taskHandle(nullptr),
            startTime(0),
            totalStackSize(0),
            #if TM_ENABLE_WATCHDOG
            watchdogFeedInterval(0),
            watchdogMissedFeeds(0),
            lastWatchdogFeed(0),
            watchdogMissThreshold(5),
            watchdogMissAction(WatchdogAction::WARN_ONLY),
            watchdogEnabled(0),
            watchdogCritical(0),
            watchdogThresholdExceeded(0),
            #endif
            isPinned(0),
            coreID(0),
            hasStackWarning(0),
            hasCrashed(0),
            reserved(0),
            stackHighWaterMark(0) {
            memset(taskName, 0, sizeof(taskName));
            #if TM_ENABLE_TASK_NAMES
            memset(abbreviatedName, 0, sizeof(abbreviatedName));
            #endif
        }
        
        // Copy constructor
        TaskInfo(const TaskInfo& other) :
            taskHandle(other.taskHandle),
            startTime(other.startTime),
            totalStackSize(other.totalStackSize),
            #if TM_ENABLE_WATCHDOG
            watchdogFeedInterval(other.watchdogFeedInterval),
            watchdogMissedFeeds(other.watchdogMissedFeeds.load()),
            lastWatchdogFeed(other.lastWatchdogFeed.load()),
            watchdogMissThreshold(other.watchdogMissThreshold),
            watchdogMissAction(other.watchdogMissAction),
            watchdogEnabled(other.watchdogEnabled),
            watchdogCritical(other.watchdogCritical),
            watchdogThresholdExceeded(other.watchdogThresholdExceeded),
            #endif
            isPinned(other.isPinned),
            coreID(other.coreID),
            hasStackWarning(other.hasStackWarning),
            hasCrashed(other.hasCrashed),
            reserved(other.reserved),
            stackHighWaterMark(other.stackHighWaterMark.load()) {
            memcpy(taskName, other.taskName, sizeof(taskName));
            #if TM_ENABLE_TASK_NAMES
            memcpy(abbreviatedName, other.abbreviatedName, sizeof(abbreviatedName));
            #endif
        }
        
        // Move constructor
        TaskInfo(TaskInfo&& other) noexcept :
            taskHandle(other.taskHandle),
            startTime(other.startTime),
            totalStackSize(other.totalStackSize),
            #if TM_ENABLE_WATCHDOG
            watchdogFeedInterval(other.watchdogFeedInterval),
            watchdogMissedFeeds(other.watchdogMissedFeeds.load()),
            lastWatchdogFeed(other.lastWatchdogFeed.load()),
            watchdogMissThreshold(other.watchdogMissThreshold),
            watchdogMissAction(other.watchdogMissAction),
            watchdogEnabled(other.watchdogEnabled),
            watchdogCritical(other.watchdogCritical),
            watchdogThresholdExceeded(other.watchdogThresholdExceeded),
            #endif
            isPinned(other.isPinned),
            coreID(other.coreID),
            hasStackWarning(other.hasStackWarning),
            hasCrashed(other.hasCrashed),
            reserved(other.reserved),
            stackHighWaterMark(other.stackHighWaterMark.load()) {
            memcpy(taskName, other.taskName, sizeof(taskName));
            #if TM_ENABLE_TASK_NAMES
            memcpy(abbreviatedName, other.abbreviatedName, sizeof(abbreviatedName));
            #endif
            // Reset other
            other.taskHandle = nullptr;
        }
        
        // Copy assignment operator
        TaskInfo& operator=(const TaskInfo& other) {
            if (this != &other) {
                taskHandle = other.taskHandle;
                memcpy(taskName, other.taskName, sizeof(taskName));
                #if TM_ENABLE_TASK_NAMES
                memcpy(abbreviatedName, other.abbreviatedName, sizeof(abbreviatedName));
                #endif
                startTime = other.startTime;
                totalStackSize = other.totalStackSize;
                #if TM_ENABLE_WATCHDOG
                watchdogFeedInterval = other.watchdogFeedInterval;
                watchdogMissedFeeds.store(other.watchdogMissedFeeds.load());
                lastWatchdogFeed.store(other.lastWatchdogFeed.load());
                watchdogMissThreshold = other.watchdogMissThreshold;
                watchdogMissAction = other.watchdogMissAction;
                watchdogEnabled = other.watchdogEnabled;
                watchdogCritical = other.watchdogCritical;
                watchdogThresholdExceeded = other.watchdogThresholdExceeded;
                #endif
                isPinned = other.isPinned;
                coreID = other.coreID;
                hasStackWarning = other.hasStackWarning;
                hasCrashed = other.hasCrashed;
                reserved = other.reserved;
                stackHighWaterMark.store(other.stackHighWaterMark.load());
            }
            return *this;
        }
        
        // Move assignment operator
        TaskInfo& operator=(TaskInfo&& other) noexcept {
            if (this != &other) {
                taskHandle = other.taskHandle;
                memcpy(taskName, other.taskName, sizeof(taskName));
                #if TM_ENABLE_TASK_NAMES
                memcpy(abbreviatedName, other.abbreviatedName, sizeof(abbreviatedName));
                #endif
                startTime = other.startTime;
                totalStackSize = other.totalStackSize;
                #if TM_ENABLE_WATCHDOG
                watchdogFeedInterval = other.watchdogFeedInterval;
                watchdogMissedFeeds.store(other.watchdogMissedFeeds.load());
                lastWatchdogFeed.store(other.lastWatchdogFeed.load());
                watchdogMissThreshold = other.watchdogMissThreshold;
                watchdogMissAction = other.watchdogMissAction;
                watchdogEnabled = other.watchdogEnabled;
                watchdogCritical = other.watchdogCritical;
                watchdogThresholdExceeded = other.watchdogThresholdExceeded;
                #endif
                isPinned = other.isPinned;
                coreID = other.coreID;
                hasStackWarning = other.hasStackWarning;
                hasCrashed = other.hasCrashed;
                reserved = other.reserved;
                stackHighWaterMark.store(other.stackHighWaterMark.load());
                // Reset other
                other.taskHandle = nullptr;
            }
            return *this;
        }
    };

    /**
     * @struct TaskStatistics
     * @brief Simplified task statistics
     */
    struct TaskStatistics {
        uint16_t totalTasks;
        uint16_t watchdogTasks;
        uint16_t pinnedTasks;
    };

    // Constructor and Destructor
    /**
     * @brief Construct TaskManager with optional watchdog injection
     * @param watchdog Watchdog implementation (nullptr uses internal NullWatchdog)
     *
     * If nullptr is passed, watchdog functionality is disabled (no-op).
     * Ownership is NOT transferred - caller maintains ownership.
     *
     * @example
     * // Use with real watchdog singleton
     * TaskManager tm(&Watchdog::getInstance());
     *
     * // Use with disabled watchdog
     * TaskManager tm(nullptr);
     *
     * // Use with mock for testing
     * MockWatchdog mock;
     * TaskManager tm(&mock);
     */
    explicit TaskManager(IWatchdog* watchdog = nullptr);
    ~TaskManager();

    // Delete copy constructor and assignment operator
    TaskManager(const TaskManager&) = delete;
    TaskManager& operator=(const TaskManager&) = delete;

    // Core task management methods
    [[nodiscard]] bool startTask(void (*taskFunction)(void*),
                   const char* taskName,
                   uint16_t stackSize,
                   void* parameter,
                   UBaseType_t priority,
                   const WatchdogConfig& watchdogConfig = WatchdogConfig::disabled());

    [[nodiscard]] bool startTaskPinned(void (*taskFunction)(void*),
                         const char* taskName,
                         uint16_t stackSize,
                         void* parameter,
                         UBaseType_t priority,
                         BaseType_t coreID,
                         const WatchdogConfig& watchdogConfig = WatchdogConfig::disabled());

    // Watchdog management (if enabled)
    #if TM_ENABLE_WATCHDOG
    [[nodiscard]] bool initWatchdog(uint32_t timeoutSeconds = 30, bool panicOnTimeout = true);
    [[nodiscard]] bool feedWatchdog();
    [[nodiscard]] bool registerCurrentTaskWithWatchdog(const char* taskName, const WatchdogConfig& config);
    [[nodiscard]] bool unregisterCurrentTaskFromWatchdog();
    void logWatchdogStats() const;
    #endif

    // Task information
    [[nodiscard]] const std::vector<TaskInfo>& getTaskList() const noexcept { return taskList_; }
    [[nodiscard]] TaskHandle_t getTaskHandleByName(const char* taskName) const;
    [[nodiscard]] TaskStatistics getTaskStatistics() const;

    // Task control
    [[nodiscard]] bool stopTask(TaskHandle_t handle);
    
    // Debug functionality (if enabled)
    #if TM_ENABLE_DEBUG_TASK
    void setResourceLogPeriod(uint32_t interval_ms);
    void debugTask();
    static void debugTaskWrapper(void* pvParameters);
    TaskHandle_t debugTaskHandle;
    #endif

    // Utility methods
    static const char* taskStateToStringShort(eTaskState taskState);
    void cleanupDeletedTasks();
    bool checkTaskStackHealth(const TaskInfo& task) const;
    void monitorTaskStacks();
    
    // Safety methods
    [[nodiscard]] bool isTaskHealthy(TaskHandle_t handle) const;
    void handleTaskFailure(TaskInfo& task);
    [[nodiscard]] bool validateTaskOperation() const;
    void enableTaskRecovery(bool enable = true);
    void setRecoveryPolicy(TaskRecovery::FailureType failure,
                          TaskRecovery::RecoveryAction action,
                          uint32_t maxRetries = 3);

    // Resource leak detection
    void enableLeakDetection(bool enable = true);
    void checkResourceLeaks();
    [[nodiscard]] std::vector<ResourceLeakDetector::TaskResourceStats> getResourceStats() const;

    // Task name lookup for compatibility
    [[nodiscard]] const char* getTaskNameByHandle(TaskHandle_t handle) const;

private:
    static const char* tag;
    std::vector<TaskInfo> taskList_;
    SemaphoreHandle_t taskListMutex;
    
    // Safety monitoring
    std::atomic<bool> isIterating_;      // Prevents concurrent modifications
    std::atomic<uint32_t> operationCount_; // Track concurrent operations
    TickType_t lastHealthCheck_;
    bool recoveryEnabled_;
    bool leakDetectionEnabled_;
    
    #if TM_ENABLE_WATCHDOG
    IWatchdog* watchdog_;           // Injected watchdog (non-owning pointer)
    NullWatchdog nullWatchdog_;     // Fallback when nullptr is passed to constructor
    bool watchdogInitialized;
    uint32_t watchdogTimeoutSeconds;
    #endif
    
    #if TM_ENABLE_DEBUG_TASK
    TickType_t resourceLogPeriod;
    #endif

    // Internal methods
    void initMutex();
    void addTaskToList(const TaskInfo& taskInfo);
    static void generalTaskWrapper(void* pvParameters);
    static void safeTaskWrapper(void* pvParameters);
    
    // Safety helpers
    bool acquireMutexSafely(TickType_t timeout = pdMS_TO_TICKS(5000));
    void releaseMutexSafely();
    std::vector<TaskInfo> copyTaskListSafely() const;
    
    #if TM_ENABLE_WATCHDOG
    bool configureTaskWatchdog(const char* taskName, const WatchdogConfig& config);
    bool configureTaskWatchdogInternal(TaskInfo& task, const WatchdogConfig& config);
    void checkWatchdogHealth();
    #endif
    
    #if TM_ENABLE_DEBUG_TASK
    void printResourceUsage();
    #endif
};

// Inline implementations for better optimization
inline TaskHandle_t TaskManager::getTaskHandleByName(const char* taskName) const {
    if (!taskName) return nullptr;
    
    for (const auto& task : taskList_) {
        if (strncmp(task.taskName, taskName, TM_MAX_TASK_NAME_LEN) == 0) {
            return task.taskHandle;
        }
    }
    return nullptr;
}

inline const char* TaskManager::getTaskNameByHandle(TaskHandle_t handle) const {
    if (!handle) return nullptr;
    
    for (const auto& task : taskList_) {
        if (task.taskHandle == handle) {
            return task.taskName;
        }
    }
    return nullptr;
}

#endif // TASKMANAGER_H