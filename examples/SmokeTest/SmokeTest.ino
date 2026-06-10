// SmokeTest.ino - Minimal compile-smoke example for ESP32-TaskManager.
//
// Exercises the core public API of TaskManager:
//   * construct the manager (with no watchdog -> internal NullWatchdog)
//   * register/start a trivial FreeRTOS task via startTask()
//   * register/start a core-pinned task via startTaskPinned()
//   * query task statistics
// The goal is to compile-link the library by default (no USE_CUSTOM_LOGGER),
// proving the duplicate-definition and missing-include bugs are fixed.

#include <Arduino.h>
#include <TaskManager.h>

TaskManager taskManager;  // nullptr watchdog -> watchdog functionality disabled

// Trivial task body: just yields periodically.
static void TrivialTask(void* param) {
    const char* name = static_cast<const char*>(param);
    for (;;) {
        Serial.printf("[%s] tick\n", name ? name : "task");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void setup() {
    Serial.begin(115200);

    // Start a plain task.
    bool ok = taskManager.startTask(
        TrivialTask,
        "smoke",
        4096,                 // stack size in bytes
        (void*)"smoke",
        1                     // priority
    );
    Serial.printf("startTask: %s\n", ok ? "ok" : "fail");

    // Start a core-pinned task.
    ok = taskManager.startTaskPinned(
        TrivialTask,
        "smokePin",
        4096,
        (void*)"smokePin",
        1,                    // priority
        0                     // core 0
    );
    Serial.printf("startTaskPinned: %s\n", ok ? "ok" : "fail");

    // Query statistics to exercise more of the API surface.
    TaskManager::TaskStatistics stats = taskManager.getTaskStatistics();
    Serial.printf("tasks=%u watchdog=%u pinned=%u\n",
                  stats.totalTasks, stats.watchdogTasks, stats.pinnedTasks);
}

void loop() {
    taskManager.monitorTaskStacks();
    delay(5000);
}
