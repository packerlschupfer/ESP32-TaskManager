# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.0] - 2025-12-04

### Added
- Initial public release
- Task lifecycle management (create, start, stop, monitor)
- Watchdog integration via IWatchdog dependency injection
- Stack high-water mark monitoring
- WatchdogConfig builder pattern for task configuration
- Task recovery policies (restart, log, ignore)
- Resource leak detection
- Task state tracking (running, suspended, deleted)
- Cleanup handler registration for resource management
- Thread-safe task enumeration with mutex protection
- Debug task for runtime monitoring (optional)
- Support for pinned and unpinned tasks
- Comprehensive error handling

Platform: ESP32 (Arduino/ESP-IDF)
License: GPL-3
Dependencies: ESP32-Watchdog, MutexGuard

### Notes
- Production-tested managing 16 concurrent tasks
- Zero watchdog resets over weeks of operation
- Previous internal versions (v1.x-v3.x) not publicly released
- Reset to v0.1.0 for clean public release start
