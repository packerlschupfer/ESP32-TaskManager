#ifndef RESOURCE_LEAK_DETECTOR_LOGGING_H
#define RESOURCE_LEAK_DETECTOR_LOGGING_H

#define RLD_LOG_TAG "LeakDetector"

// Define log levels based on debug flag
#ifdef TASKMANAGER_DEBUG
    // Debug mode: Show all levels
    #define RLD_LOG_LEVEL_E ESP_LOG_ERROR
    #define RLD_LOG_LEVEL_W ESP_LOG_WARN
    #define RLD_LOG_LEVEL_I ESP_LOG_INFO
    #define RLD_LOG_LEVEL_D ESP_LOG_DEBUG
    #define RLD_LOG_LEVEL_V ESP_LOG_VERBOSE
#else
    // Release mode: Only Error, Warn, Info
    #define RLD_LOG_LEVEL_E ESP_LOG_ERROR
    #define RLD_LOG_LEVEL_W ESP_LOG_WARN
    #define RLD_LOG_LEVEL_I ESP_LOG_INFO
    #define RLD_LOG_LEVEL_D ESP_LOG_NONE  // Suppress
    #define RLD_LOG_LEVEL_V ESP_LOG_NONE  // Suppress
#endif

// Route to custom logger or ESP-IDF
#ifdef USE_CUSTOM_LOGGER
    #include <LogInterface.h>
    #define RLD_LOG_E(...) LOG_WRITE(RLD_LOG_LEVEL_E, RLD_LOG_TAG, __VA_ARGS__)
    #define RLD_LOG_W(...) LOG_WRITE(RLD_LOG_LEVEL_W, RLD_LOG_TAG, __VA_ARGS__)
    #define RLD_LOG_I(...) LOG_WRITE(RLD_LOG_LEVEL_I, RLD_LOG_TAG, __VA_ARGS__)
    #define RLD_LOG_D(...) LOG_WRITE(RLD_LOG_LEVEL_D, RLD_LOG_TAG, __VA_ARGS__)
    #define RLD_LOG_V(...) LOG_WRITE(RLD_LOG_LEVEL_V, RLD_LOG_TAG, __VA_ARGS__)
#else
    // ESP-IDF logging with compile-time suppression
    #include <esp_log.h>
    #define RLD_LOG_E(...) ESP_LOGE(RLD_LOG_TAG, __VA_ARGS__)
    #define RLD_LOG_W(...) ESP_LOGW(RLD_LOG_TAG, __VA_ARGS__)
    #define RLD_LOG_I(...) ESP_LOGI(RLD_LOG_TAG, __VA_ARGS__)
    #ifdef TASKMANAGER_DEBUG
        #define RLD_LOG_D(...) ESP_LOGD(RLD_LOG_TAG, __VA_ARGS__)
        #define RLD_LOG_V(...) ESP_LOGV(RLD_LOG_TAG, __VA_ARGS__)
    #else
        #define RLD_LOG_D(...) ((void)0)
        #define RLD_LOG_V(...) ((void)0)
    #endif
#endif

// Resource tracking debug output
#ifdef TASKMANAGER_DEBUG
    #define RLD_LOG_ALLOC(type, ptr, size) \
        RLD_LOG_D("ALLOC: %s %p size=%u", type, ptr, size)
    #define RLD_LOG_FREE(type, ptr) \
        RLD_LOG_D("FREE: %s %p", type, ptr)
#else
    #define RLD_LOG_ALLOC(type, ptr, size) ((void)0)
    #define RLD_LOG_FREE(type, ptr) ((void)0)
#endif

#endif // RESOURCE_LEAK_DETECTOR_LOGGING_H