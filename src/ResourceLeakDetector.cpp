// ResourceLeakDetector.cpp
#include "ResourceLeakDetector.h"
#include "TaskManager.h"
#include <cstring>
#include <cstdio>

// Static member initialization
std::map<void*, ResourceLeakDetector::ResourceInfo> ResourceLeakDetector::resourceRegistry_;
std::map<TaskHandle_t, ResourceLeakDetector::TaskResourceStats> ResourceLeakDetector::taskStats_;
SemaphoreHandle_t ResourceLeakDetector::detectorMutex_ = nullptr;
bool ResourceLeakDetector::initialized_ = false;
bool ResourceLeakDetector::trackingEnabled_ = false;
TaskManager* ResourceLeakDetector::taskManager_ = nullptr;

// Implementation of getTaskNameSafe
void ResourceLeakDetector::getTaskNameSafe(TaskHandle_t task, char* buffer, size_t bufferSize) {
    if (!buffer || bufferSize == 0) return;
    
    #if INCLUDE_pcTaskGetName == 1
        // Use FreeRTOS function if available
        pcTaskGetName(task, buffer, bufferSize);
    #else
        // Fallback: Try TaskManager first, then use generic name
        bool nameFound = false;
        
        if (taskManager_ != nullptr) {
            const char* name = taskManager_->getTaskNameByHandle(task);
            if (name != nullptr) {
                strncpy(buffer, name, bufferSize - 1);
                buffer[bufferSize - 1] = '\0';
                nameFound = true;
            }
        }
        
        if (!nameFound) {
            // Use generic naming as last resort
            snprintf(buffer, bufferSize, "Task_%p", task);
        }
    #endif
}