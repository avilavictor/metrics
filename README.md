# Metrics

## Summary

This project builds a small C utility that samples the CPU and memory usage of a running Linux process and writes the results to a CSV file. It reads process data from the Linux `/proc` filesystem and logs its own activity to a text file.

The tool is useful for collecting lightweight performance metrics for a target process over time.

## Dependencies

Required tools and environment:
- C compiler with C99 support
- CMake 3.10 or newer
- A Linux environment with `/proc` support

The implementation uses standard POSIX and system headers such as `unistd.h`, `dirent.h`, `sys/stat.h`, and `sys/time.h`.

## Build

From the repository root:

```bash
mkdir -p build
cd build
cmake -DRUNTIME_OUTPUT_DIRECTORY="${PWD}/bin" ..
cmake --build .
```

The `metrics` executable is written to `build/bin`.

## Run

```bash
build/bin/metrics <process_pid> <sample_interval_ms> <process_name> <output_path>
```

Example:

```bash
mkdir -p ./results
build/bin/metrics 1234 500 metrics ./results
```

Arguments:
- `<process_pid>`: the PID of the target process to monitor
- `<sample_interval_ms>`: the interval between samples, in milliseconds
- `<process_name>`: the name used for the output files
- `<output_path>`: directory where the CSV and log files will be written

## Output

The program writes two files inside the given output directory:

- `<process_name>_metrics.csv` — sampled CPU and memory data
- `<process_name>_metrics_debug.log` — runtime log messages

CSV format:

```text
timestamp,cpu_percent,cpu_total_ns,classifier_memory_kb
```

## Behavior

- Reads CPU usage from `/proc/<pid>/stat`
- Reads memory usage from `/proc/<pid>/task/*/status`
- Samples at the requested interval until the target process exits
- Writes one row per sample with a timestamp, CPU percentage, total CPU time, and memory usage
- Logs startup, sampling, and shutdown events to the debug log file

