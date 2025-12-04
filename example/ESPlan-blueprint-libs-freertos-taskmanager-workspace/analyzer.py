#!/usr/bin/env python3
"""
TaskManager Test Results Analyzer
Analyzes serial output from the comprehensive test suite
"""

import re
import sys
from collections import defaultdict
from datetime import datetime

class TestAnalyzer:
    def __init__(self):
        self.test_results = defaultdict(list)
        self.watchdog_stats = {}
        self.memory_stats = []
        self.task_stats = defaultdict(dict)
        self.errors = []
        self.warnings = []
        
    def parse_line(self, line):
        # Extract timestamp and content
        timestamp_match = re.match(r'\[(\d+)\]\s+(\w+)\s+\((\w+)\):\s+(.*)', line)
        if not timestamp_match:
            return
            
        timestamp, level, tag, content = timestamp_match.groups()
        
        # Track errors and warnings
        if level == "ERROR":
            self.errors.append({
                'timestamp': int(timestamp),
                'tag': tag,
                'message': content
            })
        elif level == "WARN":
            self.warnings.append({
                'timestamp': int(timestamp),
                'tag': tag,
                'message': content
            })
        
        # Parse test results
        if "TEST REPORT" in content:
            self.parse_test_report(content)
        
        # Parse watchdog statistics
        if "Watchdog stats" in content:
            match = re.search(r'Total: (\d+), Missed: (\d+)', content)
            if match:
                self.watchdog_stats = {
                    'total': int(match.group(1)),
                    'missed': int(match.group(2))
                }
        
        # Parse memory statistics
        if "Free Heap:" in content:
            match = re.search(r'Free Heap: (\d+) bytes', content)
            if match:
                self.memory_stats.append({
                    'timestamp': int(timestamp),
                    'free_heap': int(match.group(1))
                })
        
        # Parse task details
        if "Stack HWM=" in content:
            self.parse_task_details(content)
    
    def parse_test_report(self, line):
        # Extract test statistics
        tests_match = re.search(r'Tests: Total=(\d+), Passed=(\d+), Failed=(\d+)', line)
        if tests_match:
            self.test_results['total'] = int(tests_match.group(1))
            self.test_results['passed'] = int(tests_match.group(2))
            self.test_results['failed'] = int(tests_match.group(3))
    
    def parse_task_details(self, line):
        # Parse task information including names
        match = re.match(r'(\w+): Stack HWM=(\d+), WDT=(\w+)(?:-(\w+))?, Names\[Full=\'([^\']+)\', FreeRTOS=\'([^\']+)\', Log=\'([^\']+)\'\]', line)
        if match:
            task_abbrev = match.group(1)
            self.task_stats[task_abbrev] = {
                'stack_hwm': int(match.group(2)),
                'watchdog': match.group(3),
                'watchdog_critical': match.group(4) == 'CRIT' if match.group(4) else False,
                'full_name': match.group(5),
                'freertos_name': match.group(6),
                'log_tag': match.group(7)
            }
    
    def analyze(self):
        print("\n" + "="*60)
        print("TaskManager Test Results Analysis")
        print("="*60)
        
        # Test summary
        if self.test_results:
            print(f"\nTest Summary:")
            print(f"  Total Tests: {self.test_results.get('total', 0)}")
            print(f"  Passed: {self.test_results.get('passed', 0)}")
            print(f"  Failed: {self.test_results.get('failed', 0)}")
            
            if self.test_results.get('total', 0) > 0:
                pass_rate = (self.test_results.get('passed', 0) / self.test_results.get('total', 0)) * 100
                print(f"  Pass Rate: {pass_rate:.1f}%")
        
        # Watchdog analysis
        if self.watchdog_stats:
            print(f"\nWatchdog Statistics:")
            print(f"  Total Feeds: {self.watchdog_stats['total']}")
            print(f"  Missed Feeds: {self.watchdog_stats['missed']}")
            if self.watchdog_stats['total'] > 0:
                miss_rate = (self.watchdog_stats['missed'] / self.watchdog_stats['total']) * 100
                print(f"  Miss Rate: {miss_rate:.2f}%")
        
        # Memory analysis
        if self.memory_stats:
            heap_values = [stat['free_heap'] for stat in self.memory_stats]
            print(f"\nMemory Statistics:")
            print(f"  Initial Free Heap: {heap_values[0]:,} bytes")
            print(f"  Final Free Heap: {heap_values[-1]:,} bytes")
            print(f"  Minimum Free Heap: {min(heap_values):,} bytes")
            print(f"  Maximum Free Heap: {max(heap_values):,} bytes")
            print(f"  Memory Lost: {heap_values[0] - heap_values[-1]:,} bytes")
        
        # Task analysis
        if self.task_stats:
            print(f"\nTask Analysis ({len(self.task_stats)} tasks):")
            
            # Check for FreeRTOS name truncation
            truncated_tasks = []
            for abbrev, stats in self.task_stats.items():
                if len(stats['freertos_name']) >= 16:
                    truncated_tasks.append(stats['full_name'])
                
                # Check stack usage
                if stats['stack_hwm'] < 200:
                    print(f"  ⚠️  Low stack warning for {abbrev}: {stats['stack_hwm']} words")
            
            if truncated_tasks:
                print(f"\n  ⚠️  Tasks with truncated FreeRTOS names:")
                for task in truncated_tasks:
                    print(f"    - {task}")
            
            # Watchdog enabled tasks
            wd_tasks = [abbrev for abbrev, stats in self.task_stats.items() 
                       if stats['watchdog'] == 'ON']
            if wd_tasks:
                print(f"\n  Watchdog-enabled tasks: {', '.join(wd_tasks)}")
                
                critical_tasks = [abbrev for abbrev, stats in self.task_stats.items() 
                                if stats.get('watchdog_critical', False)]
                if critical_tasks:
                    print(f"  Critical watchdog tasks: {', '.join(critical_tasks)}")
        
        # Error/Warning summary
        print(f"\nError/Warning Summary:")
        print(f"  Total Errors: {len(self.errors)}")
        print(f"  Total Warnings: {len(self.warnings)}")
        
        if self.errors:
            print(f"\n  Recent Errors:")
            for error in self.errors[-5:]:  # Show last 5 errors
                print(f"    [{error['tag']}] {error['message']}")
        
        if self.warnings:
            print(f"\n  Recent Warnings:")
            for warning in self.warnings[-5:]:  # Show last 5 warnings
                print(f"    [{warning['tag']}] {warning['message']}")
        
        print("\n" + "="*60)

def main():
    if len(sys.argv) > 1:
        # Read from file
        with open(sys.argv[1], 'r') as f:
            lines = f.readlines()
    else:
        # Read from stdin
        print("Reading from stdin... (Ctrl+D to end)")
        lines = sys.stdin.readlines()
    
    analyzer = TestAnalyzer()
    for line in lines:
        analyzer.parse_line(line.strip())
    
    analyzer.analyze()

if __name__ == "__main__":
    main()
