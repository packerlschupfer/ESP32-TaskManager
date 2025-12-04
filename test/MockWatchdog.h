/**
 * @file MockWatchdog.h
 * @brief Mock implementation of IWatchdog for unit testing TaskManager
 *
 * This mock allows testing TaskManager watchdog integration without
 * requiring actual hardware watchdog functionality.
 *
 * @example
 * MockWatchdog mock;
 * TaskManager tm(&mock);
 *
 * tm.initWatchdog(30, true);
 * TEST_ASSERT_EQUAL(1, mock.initCallCount);
 *
 * tm.feedWatchdog();
 * TEST_ASSERT_EQUAL(1, mock.feedCallCount);
 */

#ifndef MOCK_WATCHDOG_H
#define MOCK_WATCHDOG_H

#include "IWatchdog.h"
#include <string>
#include <cstring>

/**
 * @class MockWatchdog
 * @brief Fully instrumented mock for testing TaskManager watchdog integration
 */
class MockWatchdog : public IWatchdog {
public:
    // ============== Test Configuration ==============

    /// If false, init() returns failure
    bool initShouldSucceed = true;

    /// If false, feed() returns failure
    bool feedShouldSucceed = true;

    /// If false, registerCurrentTask() returns failure
    bool registerShouldSucceed = true;

    /// If false, unregisterCurrentTask() returns failure
    bool unregisterShouldSucceed = true;

    /// Number of unhealthy tasks to report from checkHealth()
    size_t unhealthyTaskCount = 0;

    // ============== Call Counters ==============

    int initCallCount = 0;
    int deinitCallCount = 0;
    int feedCallCount = 0;
    int registerCallCount = 0;
    int unregisterCallCount = 0;
    int unregisterByHandleCallCount = 0;
    int checkHealthCallCount = 0;
    int isInitializedCallCount = 0;
    int getTimeoutMsCallCount = 0;
    int getRegisteredTaskCountCallCount = 0;

    // ============== Call Parameters ==============

    /// Last taskName passed to registerCurrentTask
    char lastRegisteredTaskName[32] = {0};

    /// Last isCritical value passed to registerCurrentTask
    bool lastIsCritical = false;

    /// Last feedIntervalMs passed to registerCurrentTask
    uint32_t lastFeedIntervalMs = 0;

    /// Last timeoutSeconds passed to init
    uint32_t lastTimeoutSeconds = 0;

    /// Last panicOnTimeout passed to init
    bool lastPanicOnTimeout = false;

    // ============== Helper Methods ==============

    /**
     * @brief Reset all counters and state to initial values
     */
    void reset() {
        initCallCount = 0;
        deinitCallCount = 0;
        feedCallCount = 0;
        registerCallCount = 0;
        unregisterCallCount = 0;
        unregisterByHandleCallCount = 0;
        checkHealthCallCount = 0;
        isInitializedCallCount = 0;
        getTimeoutMsCallCount = 0;
        getRegisteredTaskCountCallCount = 0;

        memset(lastRegisteredTaskName, 0, sizeof(lastRegisteredTaskName));
        lastIsCritical = false;
        lastFeedIntervalMs = 0;
        lastTimeoutSeconds = 0;
        lastPanicOnTimeout = false;

        initialized_ = false;
        timeoutMs_ = 0;
        registeredTaskCount_ = 0;

        // Reset configuration to defaults
        initShouldSucceed = true;
        feedShouldSucceed = true;
        registerShouldSucceed = true;
        unregisterShouldSucceed = true;
        unhealthyTaskCount = 0;
    }

    /**
     * @brief Check if a specific method was called
     * @param method Method name ("init", "feed", "register", etc.)
     * @return true if method was called at least once
     */
    bool wasMethodCalled(const char* method) const {
        if (strcmp(method, "init") == 0) return initCallCount > 0;
        if (strcmp(method, "deinit") == 0) return deinitCallCount > 0;
        if (strcmp(method, "feed") == 0) return feedCallCount > 0;
        if (strcmp(method, "register") == 0) return registerCallCount > 0;
        if (strcmp(method, "unregister") == 0) return unregisterCallCount > 0;
        if (strcmp(method, "checkHealth") == 0) return checkHealthCallCount > 0;
        return false;
    }

    // ============== IWatchdog Implementation ==============

    bool init(uint32_t timeoutSeconds, bool panicOnTimeout) noexcept override {
        initCallCount++;
        lastTimeoutSeconds = timeoutSeconds;
        lastPanicOnTimeout = panicOnTimeout;

        if (initShouldSucceed) {
            initialized_ = true;
            timeoutMs_ = timeoutSeconds * 1000;
        }
        return initShouldSucceed;
    }

    bool deinit() noexcept override {
        deinitCallCount++;
        initialized_ = false;
        registeredTaskCount_ = 0;
        return true;
    }

    bool registerCurrentTask(const char* taskName, bool isCritical,
                            uint32_t feedIntervalMs) noexcept override {
        registerCallCount++;

        if (taskName) {
            strncpy(lastRegisteredTaskName, taskName, sizeof(lastRegisteredTaskName) - 1);
            lastRegisteredTaskName[sizeof(lastRegisteredTaskName) - 1] = '\0';
        }
        lastIsCritical = isCritical;
        lastFeedIntervalMs = feedIntervalMs;

        if (registerShouldSucceed) {
            registeredTaskCount_++;
        }
        return registerShouldSucceed;
    }

    bool unregisterCurrentTask() noexcept override {
        unregisterCallCount++;
        if (unregisterShouldSucceed && registeredTaskCount_ > 0) {
            registeredTaskCount_--;
        }
        return unregisterShouldSucceed;
    }

    bool unregisterTaskByHandle(TaskHandle_t, const char*) noexcept override {
        unregisterByHandleCallCount++;
        if (unregisterShouldSucceed && registeredTaskCount_ > 0) {
            registeredTaskCount_--;
        }
        return unregisterShouldSucceed;
    }

    bool feed() noexcept override {
        feedCallCount++;
        return feedShouldSucceed;
    }

    size_t checkHealth() noexcept override {
        checkHealthCallCount++;
        return unhealthyTaskCount;
    }

    bool isInitialized() const noexcept override {
        const_cast<MockWatchdog*>(this)->isInitializedCallCount++;
        return initialized_;
    }

    uint32_t getTimeoutMs() const noexcept override {
        const_cast<MockWatchdog*>(this)->getTimeoutMsCallCount++;
        return timeoutMs_;
    }

    size_t getRegisteredTaskCount() const noexcept override {
        const_cast<MockWatchdog*>(this)->getRegisteredTaskCountCallCount++;
        return registeredTaskCount_;
    }

private:
    bool initialized_ = false;
    uint32_t timeoutMs_ = 0;
    size_t registeredTaskCount_ = 0;
};

#endif // MOCK_WATCHDOG_H
