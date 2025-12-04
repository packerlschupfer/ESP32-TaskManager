// PriorityInversion.h - Priority inversion protection for TaskManager
#ifndef PRIORITY_INVERSION_H
#define PRIORITY_INVERSION_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <map>
#include <vector>
#include <cstdio>  // for snprintf

// Use centralized logging configuration
#include "PriorityInversionLogging.h"

/**
 * @class PriorityInversionProtection
 * @brief Detects and mitigates priority inversion scenarios
 * 
 * @note This class requires INCLUDE_pcTaskGetName to be set to 1 in FreeRTOSConfig.h
 *       for task name logging. Without it, task monitoring features will be limited.
 */
class PriorityInversionProtection {
public:
    struct MutexInfo {
        SemaphoreHandle_t mutex;
        TaskHandle_t owner;
        UBaseType_t ownerOriginalPriority;
        std::vector<TaskHandle_t> waitingTasks;
        TickType_t acquireTime;
        bool priorityBoosted;
        
        MutexInfo() : 
            mutex(nullptr), 
            owner(nullptr), 
            ownerOriginalPriority(0),
            acquireTime(0),
            priorityBoosted(false) {}
    };
    
    /**
     * @brief Priority-aware mutex that prevents priority inversion
     */
    class PriorityMutex {
    private:
        SemaphoreHandle_t mutex_;
        static std::map<SemaphoreHandle_t, MutexInfo> mutexRegistry_;
        static SemaphoreHandle_t registryMutex_;
        const char* name_;
        
    public:
        PriorityMutex(const char* name = "unnamed") : name_(name) {
            // Create mutex with priority inheritance
            mutex_ = xSemaphoreCreateMutex();
            
            if (mutex_ != nullptr) {
                // Register mutex
                if (registryMutex_ == nullptr) {
                    registryMutex_ = xSemaphoreCreateMutex();
                }
                
                if (xSemaphoreTake(registryMutex_, portMAX_DELAY) == pdTRUE) {
                    MutexInfo info;
                    info.mutex = mutex_;
                    mutexRegistry_[mutex_] = info;
                    xSemaphoreGive(registryMutex_);
                }
            }
        }
        
        ~PriorityMutex() {
            if (mutex_ != nullptr) {
                // Unregister mutex
                if (xSemaphoreTake(registryMutex_, portMAX_DELAY) == pdTRUE) {
                    mutexRegistry_.erase(mutex_);
                    xSemaphoreGive(registryMutex_);
                }
                vSemaphoreDelete(mutex_);
            }
        }
        
        bool take(TickType_t timeout = portMAX_DELAY) {
            if (mutex_ == nullptr) return false;
            
            TaskHandle_t currentTask = xTaskGetCurrentTaskHandle();
            UBaseType_t currentPriority = uxTaskPriorityGet(currentTask);
            
            // Check if we need to wait
            if (xSemaphoreTake(mutex_, 0) != pdTRUE) {
                // Mutex is taken, check for priority inversion
                checkAndBoostPriority(currentTask, currentPriority);
                
                // Now wait for mutex
                if (xSemaphoreTake(mutex_, timeout) != pdTRUE) {
                    // Failed to get mutex, remove from waiting list
                    removeFromWaitingList(currentTask);
                    return false;
                }
            }
            
            // We got the mutex, update registry
            updateOwnership(currentTask, currentPriority);
            return true;
        }
        
        void give() {
            if (mutex_ == nullptr) return;
            
            TaskHandle_t currentTask = xTaskGetCurrentTaskHandle();
            
            // Restore priority if it was boosted
            restoreOriginalPriority(currentTask);
            
            // Give mutex
            xSemaphoreGive(mutex_);
            
            // Check if any high priority tasks are waiting
            boostNextWaitingTask();
        }
        
        SemaphoreHandle_t getNativeMutex() const { return mutex_; }
        
    private:
        void checkAndBoostPriority(TaskHandle_t waitingTask, UBaseType_t waitingPriority) {
            if (xSemaphoreTake(registryMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
                auto it = mutexRegistry_.find(mutex_);
                if (it != mutexRegistry_.end()) {
                    MutexInfo& info = it->second;
                    
                    // Add to waiting list
                    info.waitingTasks.push_back(waitingTask);
                    
                    // Check if owner has lower priority
                    if (info.owner != nullptr) {
                        UBaseType_t ownerCurrentPriority = uxTaskPriorityGet(info.owner);
                        
                        if (waitingPriority > ownerCurrentPriority) {
                            // Priority inversion detected!
                            PI_LOG_W("Boosting task priority to prevent inversion on mutex %s",
                                      name_);
                            
                            // Boost owner priority
                            vTaskPrioritySet(info.owner, waitingPriority);
                            info.priorityBoosted = true;
                        }
                    }
                }
                xSemaphoreGive(registryMutex_);
            }
        }
        
        void updateOwnership(TaskHandle_t newOwner, UBaseType_t originalPriority) {
            if (xSemaphoreTake(registryMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
                auto it = mutexRegistry_.find(mutex_);
                if (it != mutexRegistry_.end()) {
                    MutexInfo& info = it->second;
                    info.owner = newOwner;
                    info.ownerOriginalPriority = originalPriority;
                    info.acquireTime = xTaskGetTickCount();
                    info.priorityBoosted = false;
                    
                    // Remove from waiting list
                    auto waitIt = std::find(info.waitingTasks.begin(), 
                                           info.waitingTasks.end(), 
                                           newOwner);
                    if (waitIt != info.waitingTasks.end()) {
                        info.waitingTasks.erase(waitIt);
                    }
                }
                xSemaphoreGive(registryMutex_);
            }
        }
        
        void restoreOriginalPriority(TaskHandle_t task) {
            if (xSemaphoreTake(registryMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
                auto it = mutexRegistry_.find(mutex_);
                if (it != mutexRegistry_.end()) {
                    MutexInfo& info = it->second;
                    
                    if (info.owner == task && info.priorityBoosted) {
                        // Restore original priority
                        vTaskPrioritySet(task, info.ownerOriginalPriority);
                        info.priorityBoosted = false;
                        
                        PI_LOG_I("Restored original priority after mutex release");
                    }
                    
                    info.owner = nullptr;
                }
                xSemaphoreGive(registryMutex_);
            }
        }
        
        void removeFromWaitingList(TaskHandle_t task) {
            if (xSemaphoreTake(registryMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
                auto it = mutexRegistry_.find(mutex_);
                if (it != mutexRegistry_.end()) {
                    MutexInfo& info = it->second;
                    auto waitIt = std::find(info.waitingTasks.begin(), 
                                           info.waitingTasks.end(), 
                                           task);
                    if (waitIt != info.waitingTasks.end()) {
                        info.waitingTasks.erase(waitIt);
                    }
                }
                xSemaphoreGive(registryMutex_);
            }
        }
        
        void boostNextWaitingTask() {
            if (xSemaphoreTake(registryMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
                auto it = mutexRegistry_.find(mutex_);
                if (it != mutexRegistry_.end()) {
                    MutexInfo& info = it->second;
                    
                    // Find highest priority waiting task
                    TaskHandle_t highestPriorityTask = nullptr;
                    UBaseType_t highestPriority = 0;
                    
                    for (auto task : info.waitingTasks) {
                        UBaseType_t priority = uxTaskPriorityGet(task);
                        if (priority > highestPriority) {
                            highestPriority = priority;
                            highestPriorityTask = task;
                        }
                    }
                    
                    // If mutex has new owner, check for priority inversion
                    if (info.owner != nullptr && highestPriorityTask != nullptr) {
                        UBaseType_t ownerPriority = uxTaskPriorityGet(info.owner);
                        if (highestPriority > ownerPriority) {
                            vTaskPrioritySet(info.owner, highestPriority);
                            info.priorityBoosted = true;
                        }
                    }
                }
                xSemaphoreGive(registryMutex_);
            }
        }
    };
    
    /**
     * @brief Monitor system for priority inversions
     */
    static void monitorPriorityInversions() {
        if (PriorityMutex::registryMutex_ == nullptr) return;
        
        if (xSemaphoreTake(PriorityMutex::registryMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
            TickType_t now = xTaskGetTickCount();
            
            for (auto& pair : PriorityMutex::mutexRegistry_) {
                MutexInfo& info = pair.second;
                
                if (info.owner != nullptr && !info.waitingTasks.empty()) {
                    // Check hold time
                    TickType_t holdTime = now - info.acquireTime;
                    if (holdTime > pdMS_TO_TICKS(5000)) { // 5 seconds
                        char taskName[configMAX_TASK_NAME_LEN];
                        #if INCLUDE_pcTaskGetName == 1
                            pcTaskGetName(info.owner, taskName, sizeof(taskName));
                        #else
                            snprintf(taskName, sizeof(taskName), "Task_%p", info.owner);
                        #endif
                        
                        PI_LOG_W("Task %s holding mutex for %u ms with %u tasks waiting",
                                  taskName, 
                                  pdTICKS_TO_MS(holdTime),
                                  info.waitingTasks.size());
                    }
                }
            }
            
            xSemaphoreGive(PriorityMutex::registryMutex_);
        }
    }
};

// Static member definitions
std::map<SemaphoreHandle_t, PriorityInversionProtection::MutexInfo> 
    PriorityInversionProtection::PriorityMutex::mutexRegistry_;
SemaphoreHandle_t PriorityInversionProtection::PriorityMutex::registryMutex_ = nullptr;

#endif // PRIORITY_INVERSION_H