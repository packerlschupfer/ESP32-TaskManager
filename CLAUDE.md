# TaskManager - CLAUDE.md

## Overview
Optimized FreeRTOS task manager with watchdog integration, stack monitoring, and 80% smaller memory footprint. Supports dependency injection for watchdog via IWatchdog interface.

## Key Features
- Task lifecycle management (create, stop, monitor)
- Watchdog integration via IWatchdog DI
- Stack monitoring with high-water mark tracking
- WatchdogConfig builder pattern
- Task recovery policies
- Resource leak detection
- Debug task for runtime monitoring

## Architecture

### Watchdog Integration (v3.0.0)
```cpp
// With real watchdog
TaskManager tm(&Watchdog::getInstance());

// Without watchdog (uses NullWatchdog internally)
TaskManager tm(nullptr);

// With mock for testing
MockWatchdog mock;
TaskManager tm(&mock);
```

### WatchdogConfig Builder
```cpp
auto config = WatchdogConfig::enabled(true, 5000)  // critical, 5s interval
    .withThreshold(3)           // 3 misses before action
    .withAction(WatchdogAction::RESTART_TASK);

tm.startTask(myTask, "Task", 4096, nullptr, 5, config);
```

## Usage
```cpp
TaskManager tm(&Watchdog::getInstance());
tm.initWatchdog(30, true);  // 30s timeout, panic on trigger

tm.startTask(myTask, "MyTask", 4096, nullptr, 5,
    WatchdogConfig::enabled(true));

tm.startTaskPinned(coreTask, "CoreTask", 4096, nullptr, 5, 1,
    WatchdogConfig::enabled(false, 10000));
```

## Configuration Flags
```cpp
#define TM_ENABLE_WATCHDOG      1  // Watchdog support
#define TM_ENABLE_DEBUG_TASK    1  // Debug monitoring task
#define TM_ENABLE_TASK_NAMES    1  // Full task name tracking
#define TM_MAX_TASK_NAME_LEN   16  // Task name length
```

## Thread Safety
- Mutex-protected task list
- Atomic counters for operations
- Safe iteration with copy-on-read

## Build Configuration
```ini
build_flags =
    -DTASKMANAGER_DEBUG  ; Enable debug logging
```
