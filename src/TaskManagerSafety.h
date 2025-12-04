// TaskManagerSafety.h - Safety utilities for TaskManager
#ifndef TASKMANAGER_SAFETY_H
#define TASKMANAGER_SAFETY_H

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <atomic>
#include <chrono>

// MutexGuard is now provided by workspace_Class-MutexGuard library
// which is included via TaskManager.h

// RecursiveMutexGuard is now provided by workspace_Class-MutexGuard library
// which is included via TaskManager.h

/**
 * @class StackMonitor
 * @brief Runtime stack overflow detection
 */
class StackMonitor {
public:
    static constexpr uint32_t STACK_WARNING_THRESHOLD = 512;  // Bytes
    static constexpr uint32_t STACK_CRITICAL_THRESHOLD = 256; // Bytes
    
    struct StackStatus {
        uint32_t freeBytes;
        uint32_t totalBytes;
        uint32_t usedBytes;
        uint32_t highWaterMark;
        bool warningLevel;
        bool criticalLevel;
    };
    
    static StackStatus checkTaskStack(TaskHandle_t task) {
        StackStatus status = {};
        
        if (task == nullptr) {
            task = xTaskGetCurrentTaskHandle();
        }
        
        // Get high water mark (minimum free stack ever)
        status.highWaterMark = uxTaskGetStackHighWaterMark(task) * sizeof(StackType_t);
        status.freeBytes = status.highWaterMark;
        
        // Get total stack size if available
        #if configRECORD_STACK_HIGH_ADDRESS
        // This would give us total stack size, but not always available
        #endif
        
        // Check warning levels
        status.warningLevel = (status.freeBytes < STACK_WARNING_THRESHOLD);
        status.criticalLevel = (status.freeBytes < STACK_CRITICAL_THRESHOLD);
        
        return status;
    }
    
    static bool isStackSafe(TaskHandle_t task = nullptr) {
        StackStatus status = checkTaskStack(task);
        return !status.criticalLevel;
    }
};

/**
 * @class DeadlockDetector
 * @brief Simple deadlock detection for mutex operations
 * 
 * @note Requires INCLUDE_pcTaskGetName to be set to 1 in FreeRTOSConfig.h
 *       for task name logging. Without it, generic task names will be used.
 */
class DeadlockDetector {
    static constexpr TickType_t DEFAULT_TIMEOUT = pdMS_TO_TICKS(5000); // 5 seconds
    
public:
    static bool takeMutexWithDeadlockDetection(SemaphoreHandle_t mutex, 
                                                TickType_t timeout = DEFAULT_TIMEOUT,
                                                const char* mutexName = "unknown") {
        if (mutex == nullptr) return false;
        
        #ifdef DEBUG_DEADLOCK_DETECTION
        TickType_t startTime = xTaskGetTickCount();
        #endif
        
        BaseType_t result = xSemaphoreTake(mutex, timeout);
        
        if (result != pdTRUE) {
            // Log potential deadlock
            #ifdef DEBUG_DEADLOCK_DETECTION
            TickType_t waitTime = xTaskGetTickCount() - startTime;
            char taskName[configMAX_TASK_NAME_LEN];
            #if INCLUDE_pcTaskGetName == 1
                pcTaskGetName(nullptr, taskName, sizeof(taskName));
            #else
                TaskHandle_t currentTask = xTaskGetCurrentTaskHandle();
                snprintf(taskName, sizeof(taskName), "Task_%p", currentTask);
            #endif
            
            // This would be logged: "Task [taskName] failed to acquire mutex [mutexName] after [waitTime]ms"
            (void)waitTime; // Suppress unused warning when logging is disabled
            #endif
            return false;
        }
        
        return true;
    }
};

/**
 * @struct AtomicCounter
 * @brief Thread-safe counter for statistics
 */
template<typename T>
class AtomicCounter {
public:
    AtomicCounter(T initial = 0) : value_(initial) {}
    
    T increment() { return value_.fetch_add(1) + 1; }
    T decrement() { return value_.fetch_sub(1) - 1; }
    T get() const { return value_.load(); }
    void set(T val) { value_.store(val); }
    
private:
    std::atomic<T> value_;
};

/**
 * @class MemoryBarrier
 * @brief Cross-core memory synchronization
 */
class MemoryBarrier {
public:
    static void full() {
        // Full memory barrier
        __sync_synchronize();
    }
    
    static void read() {
        // Read memory barrier
        __asm__ __volatile__("" : : : "memory");
    }
    
    static void write() {
        // Write memory barrier
        __asm__ __volatile__("" : : : "memory");
    }
};

#endif // TASKMANAGER_SAFETY_H