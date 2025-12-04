---
# TaskManager

`TaskManager` is a C++ class designed to manage the lifecycle of FreeRTOS tasks. It provides utilities to create, monitor, and debug tasks with support for both dynamic and static memory allocation. 

**Now includes an optimized version that reduces memory usage by up to 80% for memory-constrained applications!**

---

## Features

- **Task Creation**
  - Supports tasks with parameters, without parameters, or as member functions.
  - Allows pinning tasks to specific CPU cores (e.g., for ESP32).
  - Provides both dynamic and static memory allocation for tasks.
  - Optional abbreviated task names for compact logging.

- **Task Monitoring**
  - Logs task states, system resource usage, and runtime statistics.
  - Tracks and notifies state changes for tasks.
  - Debug tasks to log information periodically.

- **Watchdog Timer Integration**
  - Uses external Watchdog library for robust implementation
  - Per-task watchdog configuration
  - Critical task monitoring with automatic reset
  - Tasks must register from their own context for thread safety

- **Thread Safety**
  - Uses FreeRTOS semaphores to ensure thread-safe operations on internal data structures.

- **Memory Optimization (NEW)**
  - Optimized version available with 80% smaller memory footprint
  - Configurable features to reduce code size
  - Fixed-size data structures to minimize heap fragmentation

---

## Getting Started

### Prerequisites

- **Platform**: FreeRTOS-compatible environment (e.g., ESP32).
- **Dependencies**: 
  - `freertos/FreeRTOS.h`, `freertos/task.h`, `freertos/semphr.h`
  - `Logger` class for logging (optional - see Logging Configuration)
  - `MutexGuard` class for RAII mutex management
  - `Watchdog` class (v2.0.0+) for watchdog timer support - now uses singleton pattern
- **FreeRTOS Configuration**:
  - For advanced safety features (resource leak detection, priority inversion protection), enable:
    ```c
    #define INCLUDE_pcTaskGetName 1  // in FreeRTOSConfig.h
    ```
  - Core functionality works without this setting, but some safety features will be disabled.

---

## Logging Configuration

This library supports flexible logging configuration with per-library debug control:

### Using ESP-IDF Logging (Default)
No configuration needed. The library will use ESP-IDF logging.

### Using Custom Logger
Define `USE_CUSTOM_LOGGER` in your build flags:
```ini
build_flags = -DUSE_CUSTOM_LOGGER
```

### Debug Logging
To enable debug/verbose logging for TaskManager:
```ini
build_flags = -DTASKMANAGER_DEBUG
```

### Complete Example
```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino

# Production mode (no debug)
build_flags = -DUSE_CUSTOM_LOGGER

[env:esp32dev_debug]
platform = espressif32
board = esp32dev
framework = arduino

# Debug mode with custom logger
build_flags = 
    -DUSE_CUSTOM_LOGGER  ; Use custom logger
    -DTASKMANAGER_DEBUG  ; Enable debug for TaskManager
```

### Debug Features
When `TASKMANAGER_DEBUG` is enabled:
- Debug (D) and Verbose (V) log levels are active
- Performance timing macros available
- Feature-specific debug output:
  - `TASKMANAGER_DEBUG_WATCHDOG` - Watchdog operations
  - `TASKMANAGER_DEBUG_STACK` - Stack usage tracking
  - `TASKMANAGER_DEBUG_LIFECYCLE` - Task lifecycle events

---

## Class Overview

### Public Methods

1. **Task Creation**
   - `startTask`: Starts a task with or without parameters.
   - `startTaskPinned`: Starts a task pinned to a specific CPU core.
   - `startTaskWithoutParam`: Starts a task without any parameters.
   - `startStaticTask`: Starts a statically allocated task.
   - `startStaticTaskWithParam`: Statically allocated task with parameters.
   - `startStaticTaskPinned`: Statically allocated task pinned to a core.
   - `startStaticMemberFunctionTask`: Member function task with static allocation.

2. **Monitoring**
   - `getTaskList`: Retrieves the list of managed tasks.
   - `debugTask`: Periodically logs task and system information.
   - `setDebugInterval`: Sets the interval for debug logging.

3. **Utility Functions**
   - `taskStateToString`: Converts task states to human-readable strings.
   - `taskStateToStringShort`: Short string representation for task states.
   - `schedulerStateToString`: Converts scheduler states to readable strings.
   - `taskNameFromHandle`: Retrieves a task name using its handle.
   - `getTaskHandleByName`: Gets a task handle by its name.

### Internal Methods (Private)
- `addTaskToList`: Adds a task to the internal task list safely.
- `logTaskInfo`: Logs detailed task-specific information.
- `updateTaskState`: Updates and logs state changes for a task.
- `printExtensiveResourceUsage`: Logs detailed system resource usage.
- Mutex-protected access to internal task list.

---

## Example Usage

### Using the Optimized Version

```cpp
// Option 1: Define before including
#define USE_OPTIMIZED_TASKMANAGER 1
#include "TaskManagerConfig.h"

// Option 2: Use predefined configurations
#define TASKMANAGER_BALANCED  // Balanced features/size
#include "TaskManagerConfig.h"

// Option 3: Use minimal configuration
#define TASKMANAGER_MINIMAL   // Smallest footprint
#include "TaskManagerConfig.h"
```

### Creating a Task
```cpp
#include "TaskManager.h"

void exampleTaskFunction(void* params) {
    while (true) {
        // Task logic here
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

TaskManager taskManager;

void setup() {
    taskManager.startTask(exampleTaskFunction, "ExampleTask", 2048, nullptr, 1);
}
```

### Using Watchdog Timer
```cpp
// Initialize watchdog
taskManager.initWatchdog(30, true);  // 30 second timeout, panic on timeout

// Create task with watchdog
TaskManager::WatchdogConfig wdConfig = TaskManager::WatchdogConfig::enabled(true, 5000);
taskManager.startTask(myTask, "CriticalTask", 4096, nullptr, 1, wdConfig);

// In task: Feed watchdog
void myTask(void* params) {
    while (true) {
        taskManager.feedWatchdog();
        // Task work...
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```

### Debugging Tasks
```cpp
taskManager.startTask(&TaskManager::debugTaskWrapper, "DebugTask", 4096, 1);
taskManager.setDebugInterval(pdMS_TO_TICKS(10000)); // Logs every 10 seconds
```

### Statically Allocated Task
```cpp
StackType_t taskStack[256];
StaticTask_t taskTCB;

taskManager.startStaticTask(exampleTaskFunction, "StaticTask", taskStack, &taskTCB, 256, 2, "STA");
```

---

## Logging Example

`TaskManager` integrates with a logging system (`Logger` class). Example log output:
```
[INFO] TaskManager: Debug task started
[DEBUG] TaskManager: ExampleTask=RUN(1),T:1024ms,Stack:80%,Core:NP
[INFO] TaskManager: Heap: Total=81920, Free=65536, Min Free=64000
```

---

## Thread Safety

`TaskManager` ensures thread-safe operations using FreeRTOS semaphores for its internal data structures (e.g., `taskList_`).

---

## Memory Usage Comparison

| Version | TaskInfo Size | Per Task RAM | Code Size | Features |
|---------|---------------|--------------|-----------|----------|
| Original | ~140 bytes | ~200 bytes | ~28KB | Full features |
| Optimized (Full) | ~32 bytes | ~40 bytes | ~12KB | All essential features |
| Optimized (Balanced) | ~28 bytes | ~35 bytes | ~10KB | Watchdog + Debug |
| Optimized (Minimal) | ~20 bytes | ~25 bytes | ~6KB | Core features only |

## Configuration Options

The optimized version provides compile-time flags to enable/disable features:

```cpp
#define TM_ENABLE_WATCHDOG      1  // Watchdog support
#define TM_ENABLE_DEBUG_TASK    1  // Debug task functionality
#define TM_ENABLE_TASK_NAMES    1  // Abbreviated task names
#define TM_ENABLE_STATE_TRACKING 0 // Task state change tracking
#define TM_ENABLE_STATIC_TASKS  0  // Static task allocation
#define TM_ENABLE_MEMBER_TASKS  0  // Member function tasks
```

## Migration from Original to Optimized

The optimized version maintains API compatibility for core features:
- Basic task creation methods remain unchanged
- Watchdog functionality works identically
- Task lookup and statistics preserved

Removed features (rarely used):
- TaskNames struct (use const char* instead)
- Complex state change notifications
- Static task creation methods
- Member function task wrappers

## Breaking Changes

### Version 2.2.0 - Stack Size Fix
**Important**: Stack size parameter is now correctly interpreted as **bytes** instead of words.

Previously, TaskManager incorrectly passed the stack size directly to FreeRTOS, which interprets it as words (4 bytes on ESP32). This caused tasks to use 4x more memory than intended.

**Before v2.2.0** (incorrect - allocated 16KB):
```cpp
taskManager.startTask(myTask, "Task", 4096, nullptr, 1);  // Actually allocated 16,384 bytes!
```

**After v2.2.0** (correct - allocates 4KB):
```cpp
taskManager.startTask(myTask, "Task", 4096, nullptr, 1);  // Now correctly allocates 4,096 bytes
```

**Migration Guide**:
- If your code worked correctly before, you were unknowingly allocating 4x more memory
- To maintain the same actual stack size, divide your stack sizes by 4
- Or better: use this opportunity to reduce memory usage by keeping the same values
- Test your tasks after updating to ensure they have sufficient stack space

## License

This project follows the terms of the [GPL-3 License](LICENSE).