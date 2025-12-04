#ifndef TASK_RECOVERY_LOGGING_H
#define TASK_RECOVERY_LOGGING_H

#define TR_LOG_TAG "TaskRecovery"

// Define log levels based on debug flag
#ifdef TASKMANAGER_DEBUG
    // Debug mode: Show all levels
    #define TR_LOG_LEVEL_E ESP_LOG_ERROR
    #define TR_LOG_LEVEL_W ESP_LOG_WARN
    #define TR_LOG_LEVEL_I ESP_LOG_INFO
    #define TR_LOG_LEVEL_D ESP_LOG_DEBUG
    #define TR_LOG_LEVEL_V ESP_LOG_VERBOSE
#else
    // Release mode: Only Error, Warn, Info
    #define TR_LOG_LEVEL_E ESP_LOG_ERROR
    #define TR_LOG_LEVEL_W ESP_LOG_WARN
    #define TR_LOG_LEVEL_I ESP_LOG_INFO
    #define TR_LOG_LEVEL_D ESP_LOG_NONE  // Suppress
    #define TR_LOG_LEVEL_V ESP_LOG_NONE  // Suppress
#endif

// Route to custom logger or ESP-IDF
#ifdef USE_CUSTOM_LOGGER
    #include <LogInterface.h>
    #define TR_LOG_E(...) LOG_WRITE(TR_LOG_LEVEL_E, TR_LOG_TAG, __VA_ARGS__)
    #define TR_LOG_W(...) LOG_WRITE(TR_LOG_LEVEL_W, TR_LOG_TAG, __VA_ARGS__)
    #define TR_LOG_I(...) LOG_WRITE(TR_LOG_LEVEL_I, TR_LOG_TAG, __VA_ARGS__)
    #define TR_LOG_D(...) LOG_WRITE(TR_LOG_LEVEL_D, TR_LOG_TAG, __VA_ARGS__)
    #define TR_LOG_V(...) LOG_WRITE(TR_LOG_LEVEL_V, TR_LOG_TAG, __VA_ARGS__)
#else
    // ESP-IDF logging with compile-time suppression
    #include <esp_log.h>
    #define TR_LOG_E(...) ESP_LOGE(TR_LOG_TAG, __VA_ARGS__)
    #define TR_LOG_W(...) ESP_LOGW(TR_LOG_TAG, __VA_ARGS__)
    #define TR_LOG_I(...) ESP_LOGI(TR_LOG_TAG, __VA_ARGS__)
    #ifdef TASKMANAGER_DEBUG
        #define TR_LOG_D(...) ESP_LOGD(TR_LOG_TAG, __VA_ARGS__)
        #define TR_LOG_V(...) ESP_LOGV(TR_LOG_TAG, __VA_ARGS__)
    #else
        #define TR_LOG_D(...) ((void)0)
        #define TR_LOG_V(...) ((void)0)
    #endif
#endif

// Recovery-specific debug logging
#ifdef TASKMANAGER_DEBUG
    #define TR_LOG_RECOVERY(task, action) \
        TR_LOG_D("Recovery: Task %s - Action %s", task, action)
    #define TR_LOG_FAILURE(task, type, count) \
        TR_LOG_D("Failure: Task %s - Type %s - Count %u", task, type, count)
#else
    #define TR_LOG_RECOVERY(task, action) ((void)0)
    #define TR_LOG_FAILURE(task, type, count) ((void)0)
#endif

#endif // TASK_RECOVERY_LOGGING_H