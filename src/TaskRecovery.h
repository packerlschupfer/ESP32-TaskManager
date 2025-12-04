// TaskRecovery.h - Advanced recovery mechanisms for TaskManager
#ifndef TASK_RECOVERY_H
#define TASK_RECOVERY_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <functional>
#include <vector>
#include <cstring>    // for strcmp
#include <algorithm>  // for std::remove_if

// Use centralized logging configuration
#include "TaskRecoveryLogging.h"

/**
 * @class TaskRecovery
 * @brief Advanced task failure recovery and restart mechanisms
 */
class TaskRecovery {
public:
    enum class FailureType {
        STACK_OVERFLOW,
        WATCHDOG_TIMEOUT,
        EXCEPTION,
        MEMORY_ALLOCATION,
        DEADLOCK,
        UNKNOWN
    };
    
    enum class RecoveryAction {
        NONE,
        RESTART,
        RESTART_WITH_BACKOFF,
        SUSPEND,
        DELETE,
        ESCALATE
    };
    
    struct RecoveryPolicy {
        FailureType failureType;
        RecoveryAction action;
        uint32_t maxRetries;
        uint32_t backoffMs;
        std::function<void(const char*)> callback;
        
        RecoveryPolicy() : 
            failureType(FailureType::UNKNOWN),
            action(RecoveryAction::SUSPEND),
            maxRetries(3),
            backoffMs(1000),
            callback(nullptr) {}
    };
    
    struct TaskRecoveryInfo {
        TaskHandle_t handle;
        char taskName[configMAX_TASK_NAME_LEN];
        void (*taskFunction)(void*);
        void* taskParameter;
        uint16_t stackSize;
        UBaseType_t priority;
        BaseType_t coreID;
        bool isPinned;
        
        uint32_t failureCount;
        FailureType lastFailure;
        TickType_t lastFailureTime;
        uint32_t totalRestarts;
        
        TaskRecoveryInfo() : 
            handle(nullptr),
            taskFunction(nullptr),
            taskParameter(nullptr),
            stackSize(0),
            priority(0),
            coreID(0),
            isPinned(false),
            failureCount(0),
            lastFailure(FailureType::UNKNOWN),
            lastFailureTime(0),
            totalRestarts(0) {
            taskName[0] = '\0';
        }
    };
    
private:
    static std::vector<RecoveryPolicy> recoveryPolicies_;
    static std::vector<TaskRecoveryInfo> recoveryInfos_;
    static SemaphoreHandle_t recoveryMutex_;
    static bool initialized_;
    
public:
    static bool initialize() {
        if (!initialized_) {
            recoveryMutex_ = xSemaphoreCreateMutex();
            if (recoveryMutex_ != nullptr) {
                // Set default policies
                setDefaultPolicies();
                initialized_ = true;
            }
        }
        return initialized_;
    }
    
    static void setDefaultPolicies() {
        // Stack overflow - try restart with larger stack
        RecoveryPolicy stackPolicy;
        stackPolicy.failureType = FailureType::STACK_OVERFLOW;
        stackPolicy.action = RecoveryAction::RESTART_WITH_BACKOFF;
        stackPolicy.maxRetries = 2;
        stackPolicy.backoffMs = 2000;
        addPolicy(stackPolicy);
        
        // Watchdog timeout - restart with backoff
        RecoveryPolicy watchdogPolicy;
        watchdogPolicy.failureType = FailureType::WATCHDOG_TIMEOUT;
        watchdogPolicy.action = RecoveryAction::RESTART_WITH_BACKOFF;
        watchdogPolicy.maxRetries = 3;
        watchdogPolicy.backoffMs = 5000;
        addPolicy(watchdogPolicy);
        
        // Exception - suspend for investigation
        RecoveryPolicy exceptionPolicy;
        exceptionPolicy.failureType = FailureType::EXCEPTION;
        exceptionPolicy.action = RecoveryAction::SUSPEND;
        exceptionPolicy.maxRetries = 0;
        addPolicy(exceptionPolicy);
        
        // Memory allocation - wait and retry
        RecoveryPolicy memoryPolicy;
        memoryPolicy.failureType = FailureType::MEMORY_ALLOCATION;
        memoryPolicy.action = RecoveryAction::RESTART_WITH_BACKOFF;
        memoryPolicy.maxRetries = 5;
        memoryPolicy.backoffMs = 10000;
        addPolicy(memoryPolicy);
    }
    
    static void addPolicy(const RecoveryPolicy& policy) {
        if (xSemaphoreTake(recoveryMutex_, portMAX_DELAY) == pdTRUE) {
            recoveryPolicies_.push_back(policy);
            xSemaphoreGive(recoveryMutex_);
        }
    }
    
    static void registerTask(const TaskRecoveryInfo& info) {
        if (xSemaphoreTake(recoveryMutex_, portMAX_DELAY) == pdTRUE) {
            // Remove any existing entry
            auto it = std::remove_if(recoveryInfos_.begin(), recoveryInfos_.end(),
                [&info](const TaskRecoveryInfo& existing) {
                    return strcmp(existing.taskName, info.taskName) == 0;
                });
            recoveryInfos_.erase(it, recoveryInfos_.end());
            
            // Add new entry
            recoveryInfos_.push_back(info);
            xSemaphoreGive(recoveryMutex_);
        }
    }
    
    static RecoveryAction determineRecoveryAction(const char* taskName, FailureType failure) {
        RecoveryAction action = RecoveryAction::SUSPEND; // Default
        
        if (xSemaphoreTake(recoveryMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
            // Find matching policy
            for (const auto& policy : recoveryPolicies_) {
                if (policy.failureType == failure) {
                    action = policy.action;
                    
                    // Check retry limit
                    for (auto& info : recoveryInfos_) {
                        if (strcmp(info.taskName, taskName) == 0) {
                            if (info.failureCount >= policy.maxRetries) {
                                action = RecoveryAction::ESCALATE;
                            }
                            break;
                        }
                    }
                    break;
                }
            }
            xSemaphoreGive(recoveryMutex_);
        }
        
        return action;
    }
    
    static bool attemptRecovery(const char* taskName, FailureType failure) {
        bool recovered = false;
        
        if (xSemaphoreTake(recoveryMutex_, pdMS_TO_TICKS(1000)) == pdTRUE) {
            TaskRecoveryInfo* taskInfo = nullptr;
            RecoveryPolicy* policy = nullptr;
            
            // Find task info
            for (auto& info : recoveryInfos_) {
                if (strcmp(info.taskName, taskName) == 0) {
                    taskInfo = &info;
                    break;
                }
            }
            
            // Find policy
            for (auto& p : recoveryPolicies_) {
                if (p.failureType == failure) {
                    policy = &p;
                    break;
                }
            }
            
            if (taskInfo && policy) {
                taskInfo->failureCount++;
                taskInfo->lastFailure = failure;
                taskInfo->lastFailureTime = xTaskGetTickCount();
                
                // Log the failure
                TR_LOG_W("Task %s failed (%d): %s, attempt %lu/%lu",
                          taskName, 
                          static_cast<int>(failure),
                          failureTypeToString(failure),
                          static_cast<unsigned long>(taskInfo->failureCount),
                          static_cast<unsigned long>(policy->maxRetries));
                
                // Execute recovery based on policy
                switch (policy->action) {
                    case RecoveryAction::RESTART:
                        recovered = restartTask(taskInfo, 0);
                        break;
                        
                    case RecoveryAction::RESTART_WITH_BACKOFF:
                        recovered = restartTask(taskInfo, policy->backoffMs);
                        break;
                        
                    case RecoveryAction::SUSPEND:
                        if (taskInfo->handle) {
                            vTaskSuspend(taskInfo->handle);
                        }
                        break;
                        
                    case RecoveryAction::DELETE:
                        if (taskInfo->handle) {
                            vTaskDelete(taskInfo->handle);
                            taskInfo->handle = nullptr;
                        }
                        break;
                        
                    case RecoveryAction::ESCALATE:
                        TR_LOG_E("Task %s exceeded retry limit - escalating",
                                  taskName);
                        if (policy->callback) {
                            policy->callback(taskName);
                        }
                        break;
                        
                    default:
                        break;
                }
            }
            
            xSemaphoreGive(recoveryMutex_);
        }
        
        return recovered;
    }
    
private:
    static bool restartTask(TaskRecoveryInfo* info, uint32_t delayMs) {
        if (!info || !info->taskFunction) return false;
        
        // Delete old task if it exists
        if (info->handle) {
            vTaskDelete(info->handle);
            info->handle = nullptr;
        }
        
        // Wait before restart
        if (delayMs > 0) {
            vTaskDelay(pdMS_TO_TICKS(delayMs));
        }
        
        // Adjust stack size if needed (for stack overflow)
        uint16_t newStackSize = info->stackSize;
        if (info->lastFailure == FailureType::STACK_OVERFLOW) {
            newStackSize = (info->stackSize * 3) / 2; // Increase by 50%
            TR_LOG_I("Increasing stack for %s: %u -> %u",
                      info->taskName, info->stackSize, newStackSize);
        }
        
        // Recreate task
        BaseType_t result;
        if (info->isPinned) {
            result = xTaskCreatePinnedToCore(
                reinterpret_cast<TaskFunction_t>(info->taskFunction),
                info->taskName,
                newStackSize,
                info->taskParameter,
                info->priority,
                &info->handle,
                info->coreID
            );
        } else {
            result = xTaskCreate(
                reinterpret_cast<TaskFunction_t>(info->taskFunction),
                info->taskName,
                newStackSize,
                info->taskParameter,
                info->priority,
                &info->handle
            );
        }
        
        if (result == pdPASS) {
            info->totalRestarts++;
            info->stackSize = newStackSize; // Update if changed
            return true;
        }
        
        return false;
    }
    
    static const char* failureTypeToString(FailureType type) {
        switch (type) {
            case FailureType::STACK_OVERFLOW: return "Stack Overflow";
            case FailureType::WATCHDOG_TIMEOUT: return "Watchdog Timeout";
            case FailureType::EXCEPTION: return "Exception";
            case FailureType::MEMORY_ALLOCATION: return "Memory Allocation";
            case FailureType::DEADLOCK: return "Deadlock";
            default: return "Unknown";
        }
    }
};

// Static member definitions - moved to implementation file to avoid multiple definitions

#endif // TASK_RECOVERY_H