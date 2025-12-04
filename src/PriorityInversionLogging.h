#ifndef PRIORITY_INVERSION_LOGGING_H
#define PRIORITY_INVERSION_LOGGING_H

#define PI_LOG_TAG "PriorityInversion"

// Define log levels based on debug flag
#ifdef TASKMANAGER_DEBUG
    // Debug mode: Show all levels
    #define PI_LOG_LEVEL_E ESP_LOG_ERROR
    #define PI_LOG_LEVEL_W ESP_LOG_WARN
    #define PI_LOG_LEVEL_I ESP_LOG_INFO
    #define PI_LOG_LEVEL_D ESP_LOG_DEBUG
    #define PI_LOG_LEVEL_V ESP_LOG_VERBOSE
#else
    // Release mode: Only Error, Warn, Info
    #define PI_LOG_LEVEL_E ESP_LOG_ERROR
    #define PI_LOG_LEVEL_W ESP_LOG_WARN
    #define PI_LOG_LEVEL_I ESP_LOG_INFO
    #define PI_LOG_LEVEL_D ESP_LOG_NONE  // Suppress
    #define PI_LOG_LEVEL_V ESP_LOG_NONE  // Suppress
#endif

// Route to custom logger or ESP-IDF
#ifdef USE_CUSTOM_LOGGER
    #include <LogInterface.h>
    #define PI_LOG_E(...) LOG_WRITE(PI_LOG_LEVEL_E, PI_LOG_TAG, __VA_ARGS__)
    #define PI_LOG_W(...) LOG_WRITE(PI_LOG_LEVEL_W, PI_LOG_TAG, __VA_ARGS__)
    #define PI_LOG_I(...) LOG_WRITE(PI_LOG_LEVEL_I, PI_LOG_TAG, __VA_ARGS__)
    #define PI_LOG_D(...) LOG_WRITE(PI_LOG_LEVEL_D, PI_LOG_TAG, __VA_ARGS__)
    #define PI_LOG_V(...) LOG_WRITE(PI_LOG_LEVEL_V, PI_LOG_TAG, __VA_ARGS__)
#else
    // ESP-IDF logging with compile-time suppression
    #include <esp_log.h>
    #define PI_LOG_E(...) ESP_LOGE(PI_LOG_TAG, __VA_ARGS__)
    #define PI_LOG_W(...) ESP_LOGW(PI_LOG_TAG, __VA_ARGS__)
    #define PI_LOG_I(...) ESP_LOGI(PI_LOG_TAG, __VA_ARGS__)
    #ifdef TASKMANAGER_DEBUG
        #define PI_LOG_D(...) ESP_LOGD(PI_LOG_TAG, __VA_ARGS__)
        #define PI_LOG_V(...) ESP_LOGV(PI_LOG_TAG, __VA_ARGS__)
    #else
        #define PI_LOG_D(...) ((void)0)
        #define PI_LOG_V(...) ((void)0)
    #endif
#endif

// Priority inversion specific debug
#ifdef TASKMANAGER_DEBUG
    #define PI_LOG_BOOST(task, oldPri, newPri) \
        PI_LOG_D("Boost: Task %p priority %u -> %u", task, oldPri, newPri)
    #define PI_LOG_MUTEX(mutex, owner, waiters) \
        PI_LOG_D("Mutex %p: owner=%p, waiters=%u", mutex, owner, waiters)
#else
    #define PI_LOG_BOOST(task, oldPri, newPri) ((void)0)
    #define PI_LOG_MUTEX(mutex, owner, waiters) ((void)0)
#endif

#endif // PRIORITY_INVERSION_LOGGING_H