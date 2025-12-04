#ifndef TASKMANAGER_LOGGING_H
#define TASKMANAGER_LOGGING_H

#define TASKM_LOG_TAG "TaskManager"

// Define log levels based on debug flag
#ifdef TASKMANAGER_DEBUG
    // Debug mode: Show all levels
    #define TASKM_LOG_LEVEL_E ESP_LOG_ERROR
    #define TASKM_LOG_LEVEL_W ESP_LOG_WARN
    #define TASKM_LOG_LEVEL_I ESP_LOG_INFO
    #define TASKM_LOG_LEVEL_D ESP_LOG_DEBUG
    #define TASKM_LOG_LEVEL_V ESP_LOG_VERBOSE
#else
    // Release mode: Only Error, Warn, Info
    #define TASKM_LOG_LEVEL_E ESP_LOG_ERROR
    #define TASKM_LOG_LEVEL_W ESP_LOG_WARN
    #define TASKM_LOG_LEVEL_I ESP_LOG_INFO
    #define TASKM_LOG_LEVEL_D ESP_LOG_NONE  // Suppress
    #define TASKM_LOG_LEVEL_V ESP_LOG_NONE  // Suppress
#endif

// Route to custom logger or ESP-IDF
#ifdef USE_CUSTOM_LOGGER
    #include <LogInterface.h>
    #define TASKM_LOG_E(...) LOG_WRITE(TASKM_LOG_LEVEL_E, TASKM_LOG_TAG, __VA_ARGS__)
    #define TASKM_LOG_W(...) LOG_WRITE(TASKM_LOG_LEVEL_W, TASKM_LOG_TAG, __VA_ARGS__)
    #define TASKM_LOG_I(...) LOG_WRITE(TASKM_LOG_LEVEL_I, TASKM_LOG_TAG, __VA_ARGS__)
    #define TASKM_LOG_D(...) LOG_WRITE(TASKM_LOG_LEVEL_D, TASKM_LOG_TAG, __VA_ARGS__)
    #define TASKM_LOG_V(...) LOG_WRITE(TASKM_LOG_LEVEL_V, TASKM_LOG_TAG, __VA_ARGS__)
#else
    // ESP-IDF logging with compile-time suppression
    #include <esp_log.h>
    #define TASKM_LOG_E(...) ESP_LOGE(TASKM_LOG_TAG, __VA_ARGS__)
    #define TASKM_LOG_W(...) ESP_LOGW(TASKM_LOG_TAG, __VA_ARGS__)
    #define TASKM_LOG_I(...) ESP_LOGI(TASKM_LOG_TAG, __VA_ARGS__)
    #ifdef TASKMANAGER_DEBUG
        #define TASKM_LOG_D(...) ESP_LOGD(TASKM_LOG_TAG, __VA_ARGS__)
        #define TASKM_LOG_V(...) ESP_LOGV(TASKM_LOG_TAG, __VA_ARGS__)
    #else
        #define TASKM_LOG_D(...) ((void)0)
        #define TASKM_LOG_V(...) ((void)0)
    #endif
#endif

// Feature-specific debug flags
#ifdef TASKMANAGER_DEBUG
    #define TASKMANAGER_DEBUG_WATCHDOG    // Watchdog-related debugging
    #define TASKMANAGER_DEBUG_STACK       // Stack usage debugging
    #define TASKMANAGER_DEBUG_LIFECYCLE   // Task lifecycle debugging
#endif

// Watchdog debug logging
#ifdef TASKMANAGER_DEBUG_WATCHDOG
    #define TASKM_LOG_WD(...) TASKM_LOG_D("WD: " __VA_ARGS__)
#else
    #define TASKM_LOG_WD(...) ((void)0)
#endif

// Stack debug logging
#ifdef TASKMANAGER_DEBUG_STACK
    #define TASKM_LOG_STACK(...) TASKM_LOG_D("STACK: " __VA_ARGS__)
#else
    #define TASKM_LOG_STACK(...) ((void)0)
#endif

// Lifecycle debug logging
#ifdef TASKMANAGER_DEBUG_LIFECYCLE
    #define TASKM_LOG_LIFE(...) TASKM_LOG_D("LIFE: " __VA_ARGS__)
#else
    #define TASKM_LOG_LIFE(...) ((void)0)
#endif

// Performance timing macros
#ifdef TASKMANAGER_DEBUG
    #define TASKM_TIME_START() TickType_t _start = xTaskGetTickCount()
    #define TASKM_TIME_END(msg) TASKM_LOG_D("Timing: %s took %lu ms", msg, pdTICKS_TO_MS(xTaskGetTickCount() - _start))
#else
    #define TASKM_TIME_START() ((void)0)
    #define TASKM_TIME_END(msg) ((void)0)
#endif

#endif // TASKMANAGER_LOGGING_H