// ResourceLeakDetector.h - Resource leak detection for TaskManager
#ifndef RESOURCE_LEAK_DETECTOR_H
#define RESOURCE_LEAK_DETECTOR_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <map>
#include <vector>
#include <string>
#include <functional>  // for std::function

// Use centralized logging configuration
#include "ResourceLeakDetectorLogging.h"

// Forward declaration for TaskManager access
class TaskManager;

/**
 * @class ResourceLeakDetector
 * @brief Tracks resource allocations and detects leaks when tasks terminate
 * 
 * @note This class requires INCLUDE_pcTaskGetName to be set to 1 in FreeRTOSConfig.h
 *       for full functionality. Without it, generic task names will be used.
 */
class ResourceLeakDetector {
public:
    enum class ResourceType {
        MEMORY,
        MUTEX,
        SEMAPHORE,
        QUEUE,
        TIMER,
        EVENT_GROUP,
        STREAM_BUFFER,
        FILE_HANDLE,
        CUSTOM
    };
    
    struct ResourceInfo {
        void* resource;
        ResourceType type;
        size_t size;
        TaskHandle_t owner;
        TickType_t allocTime;
        std::string location;
        bool leaked;
        
        ResourceInfo() : 
            resource(nullptr), 
            type(ResourceType::CUSTOM),
            size(0),
            owner(nullptr),
            allocTime(0),
            leaked(false) {}
    };
    
    struct TaskResourceStats {
        TaskHandle_t handle;
        char taskName[configMAX_TASK_NAME_LEN];
        uint32_t totalAllocations;
        uint32_t totalDeallocations;
        uint32_t currentAllocations;
        size_t currentMemoryUsage;
        uint32_t leakedResources;
        size_t leakedMemory;
        
        TaskResourceStats() :
            handle(nullptr),
            totalAllocations(0),
            totalDeallocations(0),
            currentAllocations(0),
            currentMemoryUsage(0),
            leakedResources(0),
            leakedMemory(0) {
            taskName[0] = '\0';
        }
    };
    
private:
    static std::map<void*, ResourceInfo> resourceRegistry_;
    static std::map<TaskHandle_t, TaskResourceStats> taskStats_;
    static SemaphoreHandle_t detectorMutex_;
    static bool initialized_;
    static bool trackingEnabled_;
    static TaskManager* taskManager_;  // Optional reference to TaskManager for name lookup
    
public:
    static bool initialize() {
        if (!initialized_) {
            detectorMutex_ = xSemaphoreCreateMutex();
            if (detectorMutex_ != nullptr) {
                initialized_ = true;
                trackingEnabled_ = true;
            }
        }
        return initialized_;
    }
    
    static void enableTracking(bool enable) {
        trackingEnabled_ = enable;
    }
    
    static void setTaskManager(TaskManager* tm) {
        taskManager_ = tm;
    }
    
    // Helper function to get task name with fallback
    static void getTaskNameSafe(TaskHandle_t task, char* buffer, size_t bufferSize);
    
    // Resource allocation tracking
    static void trackAllocation(void* resource, ResourceType type, size_t size, 
                               const char* location = nullptr) {
        if (!trackingEnabled_ || !resource) return;
        
        if (xSemaphoreTake(detectorMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
            TaskHandle_t currentTask = xTaskGetCurrentTaskHandle();
            
            // Record resource
            ResourceInfo info;
            info.resource = resource;
            info.type = type;
            info.size = size;
            info.owner = currentTask;
            info.allocTime = xTaskGetTickCount();
            if (location) {
                info.location = location;
            }
            
            resourceRegistry_[resource] = info;
            
            // Update task stats
            auto& stats = taskStats_[currentTask];
            if (stats.handle == nullptr) {
                stats.handle = currentTask;
                getTaskNameSafe(currentTask, stats.taskName, sizeof(stats.taskName));
            }
            stats.totalAllocations++;
            stats.currentAllocations++;
            if (type == ResourceType::MEMORY) {
                stats.currentMemoryUsage += size;
            }
            
            xSemaphoreGive(detectorMutex_);
        }
    }
    
    static void trackDeallocation(void* resource) {
        if (!trackingEnabled_ || !resource) return;
        
        if (xSemaphoreTake(detectorMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
            auto it = resourceRegistry_.find(resource);
            if (it != resourceRegistry_.end()) {
                ResourceInfo& info = it->second;
                TaskHandle_t owner = info.owner;
                
                // Update task stats
                auto statsIt = taskStats_.find(owner);
                if (statsIt != taskStats_.end()) {
                    TaskResourceStats& stats = statsIt->second;
                    stats.totalDeallocations++;
                    stats.currentAllocations--;
                    if (info.type == ResourceType::MEMORY) {
                        stats.currentMemoryUsage -= info.size;
                    }
                }
                
                // Remove from registry
                resourceRegistry_.erase(it);
            }
            
            xSemaphoreGive(detectorMutex_);
        }
    }
    
    // Check for leaks when task is deleted
    static std::vector<ResourceInfo> checkTaskLeaks(TaskHandle_t task) {
        std::vector<ResourceInfo> leaks;
        
        if (xSemaphoreTake(detectorMutex_, pdMS_TO_TICKS(1000)) == pdTRUE) {
            // Find all resources owned by this task
            for (auto& pair : resourceRegistry_) {
                ResourceInfo& info = pair.second;
                if (info.owner == task && !info.leaked) {
                    info.leaked = true;
                    leaks.push_back(info);
                    
                    // Update task stats
                    auto statsIt = taskStats_.find(task);
                    if (statsIt != taskStats_.end()) {
                        TaskResourceStats& stats = statsIt->second;
                        stats.leakedResources++;
                        if (info.type == ResourceType::MEMORY) {
                            stats.leakedMemory += info.size;
                        }
                    }
                }
            }
            
            xSemaphoreGive(detectorMutex_);
        }
        
        return leaks;
    }
    
    // Report leaks for a task
    static void reportTaskLeaks(TaskHandle_t task) {
        auto leaks = checkTaskLeaks(task);
        
        if (!leaks.empty()) {
            char taskName[configMAX_TASK_NAME_LEN];
            getTaskNameSafe(task, taskName, sizeof(taskName));
            
            RLD_LOG_E("Task %s leaked %lu resources:", taskName, static_cast<unsigned long>(leaks.size()));
            
            for (const auto& leak : leaks) {
                const char* typeStr = resourceTypeToString(leak.type);
                if (!leak.location.empty()) {
                    RLD_LOG_E("  - %s (%lu bytes) at %s, held for %lu ms",
                              typeStr, static_cast<unsigned long>(leak.size), leak.location.c_str(),
                              static_cast<unsigned long>(pdTICKS_TO_MS(xTaskGetTickCount() - leak.allocTime)));
                } else {
                    RLD_LOG_E("  - %s (%lu bytes), held for %lu ms",
                              typeStr, static_cast<unsigned long>(leak.size),
                              static_cast<unsigned long>(pdTICKS_TO_MS(xTaskGetTickCount() - leak.allocTime)));
                }
            }
        }
    }
    
    // Get statistics for all tasks
    static std::vector<TaskResourceStats> getAllTaskStats() {
        std::vector<TaskResourceStats> allStats;
        
        if (xSemaphoreTake(detectorMutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
            for (const auto& pair : taskStats_) {
                allStats.push_back(pair.second);
            }
            xSemaphoreGive(detectorMutex_);
        }
        
        return allStats;
    }
    
    // Check system-wide resource health
    static void performHealthCheck() {
        if (xSemaphoreTake(detectorMutex_, pdMS_TO_TICKS(1000)) == pdTRUE) {
            // Logger instance removed - using macros instead
            
            // Count resources by type
            std::map<ResourceType, uint32_t> typeCounts;
            size_t totalMemory = 0;
            uint32_t oldestResourceAge = 0;
            
            TickType_t now = xTaskGetTickCount();
            
            for (const auto& pair : resourceRegistry_) {
                const ResourceInfo& info = pair.second;
                typeCounts[info.type]++;
                
                if (info.type == ResourceType::MEMORY) {
                    totalMemory += info.size;
                }
                
                uint32_t age = pdTICKS_TO_MS(now - info.allocTime);
                if (age > oldestResourceAge) {
                    oldestResourceAge = age;
                }
            }
            
            // Report if concerning
            if (resourceRegistry_.size() > 1000) {
                RLD_LOG_W("High resource count: %lu total resources",
                          static_cast<unsigned long>(resourceRegistry_.size()));
            }
            
            if (totalMemory > 100 * 1024) { // 100KB
                RLD_LOG_W("High tracked memory usage: %lu KB",
                          static_cast<unsigned long>(totalMemory / 1024));
            }
            
            if (oldestResourceAge > 300000) { // 5 minutes
                RLD_LOG_W("Found resources older than 5 minutes");
            }
            
            xSemaphoreGive(detectorMutex_);
        }
    }
    
    // Memory allocation wrappers
    static void* trackedMalloc(size_t size, const char* location = nullptr) {
        void* ptr = malloc(size);
        if (ptr) {
            trackAllocation(ptr, ResourceType::MEMORY, size, location);
        }
        return ptr;
    }
    
    static void trackedFree(void* ptr) {
        if (ptr) {
            trackDeallocation(ptr);
            free(ptr);
        }
    }
    
    // RAII wrapper for automatic tracking
    template<typename T>
    class TrackedResource {
    private:
        T* resource_;
        ResourceType type_;
        std::function<void(T*)> deleter_;
        
    public:
        TrackedResource(T* resource, ResourceType type, 
                       std::function<void(T*)> deleter,
                       const char* location = nullptr) 
            : resource_(resource), type_(type), deleter_(deleter) {
            if (resource_) {
                trackAllocation(resource_, type_, sizeof(T), location);
            }
        }
        
        ~TrackedResource() {
            if (resource_) {
                trackDeallocation(resource_);
                if (deleter_) {
                    deleter_(resource_);
                }
            }
        }
        
        T* get() { return resource_; }
        T* operator->() { return resource_; }
        T& operator*() { return *resource_; }
        
        // Disable copy
        TrackedResource(const TrackedResource&) = delete;
        TrackedResource& operator=(const TrackedResource&) = delete;
        
        // Enable move
        TrackedResource(TrackedResource&& other) noexcept 
            : resource_(other.resource_), type_(other.type_), deleter_(std::move(other.deleter_)) {
            other.resource_ = nullptr;
        }
    };
    
private:
    static const char* resourceTypeToString(ResourceType type) {
        switch (type) {
            case ResourceType::MEMORY: return "Memory";
            case ResourceType::MUTEX: return "Mutex";
            case ResourceType::SEMAPHORE: return "Semaphore";
            case ResourceType::QUEUE: return "Queue";
            case ResourceType::TIMER: return "Timer";
            case ResourceType::EVENT_GROUP: return "EventGroup";
            case ResourceType::STREAM_BUFFER: return "StreamBuffer";
            case ResourceType::FILE_HANDLE: return "FileHandle";
            case ResourceType::CUSTOM: return "Custom";
            default: return "Unknown";
        }
    }
};

// Static member definitions moved to ResourceLeakDetector.cpp

// Convenience macros for tracking
#define TRACKED_MALLOC(size) ResourceLeakDetector::trackedMalloc(size, __FILE__ ":" __STRING(__LINE__))
#define TRACKED_FREE(ptr) ResourceLeakDetector::trackedFree(ptr)

#endif // RESOURCE_LEAK_DETECTOR_H