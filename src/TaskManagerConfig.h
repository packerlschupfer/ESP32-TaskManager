// TaskManagerConfig.h - Configuration to select TaskManager implementation
#ifndef TASKMANAGER_CONFIG_H
#define TASKMANAGER_CONFIG_H

// Set to 1 to use the full-featured original version
// Set to 0 to use the optimized version (default)
#ifndef USE_FULL_TASKMANAGER
    #define USE_FULL_TASKMANAGER 0
#endif

// Include the appropriate implementation
#if USE_FULL_TASKMANAGER
    #include "TaskManager_Full.h"
#else
    #include "TaskManager.h"  // Optimized version is now default
#endif

// Optional: Define shortcuts for common configurations
#ifdef TASKMANAGER_MINIMAL
    // Use optimized version with minimal features
    #undef USE_FULL_TASKMANAGER
    #define USE_FULL_TASKMANAGER 0
    #define TM_ENABLE_WATCHDOG      0
    #define TM_ENABLE_DEBUG_TASK    0
    #define TM_ENABLE_TASK_NAMES    0
#endif

#ifdef TASKMANAGER_BALANCED
    // Use optimized version with balanced features
    #undef USE_FULL_TASKMANAGER
    #define USE_FULL_TASKMANAGER 0
    #define TM_ENABLE_WATCHDOG      1
    #define TM_ENABLE_DEBUG_TASK    1
    #define TM_ENABLE_TASK_NAMES    0
#endif

#ifdef TASKMANAGER_FULL_FEATURES
    // Use the original full-featured version
    #undef USE_FULL_TASKMANAGER
    #define USE_FULL_TASKMANAGER 1
#endif

#endif // TASKMANAGER_CONFIG_H