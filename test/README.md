# TaskManager Unit Tests

This directory contains comprehensive unit tests for the TaskManager library.

## Test Structure

### Core Tests (`test_taskmanager_core.cpp`)
- Basic task creation and management
- Task pinning to specific cores
- Task handle retrieval
- Invalid parameter handling
- Task naming and abbreviations
- Task statistics
- Watchdog initialization and registration
- Memory leak testing
- Log tag management

### Task Recovery Tests (`test_task_recovery.cpp`)
- Recovery policy configuration
- Crash detection and recovery
- Retry limit enforcement
- Memory usage tracking
- Recovery statistics
- Recovery callbacks
- Cleanup of deleted tasks

### Resource Leak Detection Tests (`test_resource_leak_detector.cpp`)
- Memory leak detection
- Semaphore leak detection
- Queue leak detection
- Threshold configuration
- Leak callbacks
- Statistics tracking
- Multi-task tracking

## Running the Tests

### With PlatformIO

1. From the project root:
```bash
cd test
pio test -e esp32dev
```

2. Or run specific test suites:
```bash
pio test -e esp32dev -f test_taskmanager_core
pio test -e esp32dev -f test_task_recovery
pio test -e esp32dev -f test_resource_leak_detector
```

### Integration with Your Project

1. Copy the test files to your project's `test/` directory
2. Include the TaskManager library in your `platformio.ini`:
```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
lib_deps = 
    TaskManager
test_build_src = yes
```

3. Run tests: `pio test`

## Test Coverage

The tests cover:
- ✅ Task lifecycle management
- ✅ Error handling and edge cases
- ✅ Watchdog functionality
- ✅ Memory leak detection
- ✅ Task recovery mechanisms
- ✅ Resource tracking
- ✅ Thread safety
- ✅ Configuration options
- ✅ Callback mechanisms
- ✅ Statistics and monitoring

## Adding New Tests

To add new tests:

1. Create a new test function following the pattern:
```cpp
void test_feature_name() {
    // Setup
    TaskManager tm;
    
    // Execute
    bool result = tm.someMethod();
    
    // Verify
    TEST_ASSERT_TRUE(result);
}
```

2. Add the test to the appropriate runner function:
```cpp
RUN_TEST(test_feature_name);
```

## Dependencies

- Unity test framework (included with PlatformIO)
- FreeRTOS (part of ESP32 Arduino core)
- TaskManager library

## Notes

- Tests use FreeRTOS synchronization primitives (EventGroups, Semaphores)
- Each test is isolated with proper setup/teardown
- Tests verify both success and failure scenarios
- Memory leak tests ensure proper resource cleanup