#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdarg.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>

#define METRICS_LOG_FILE "metrics_debug.log"

static char metrics_csv_file_path[512];
static char metrics_log_file_path[512];

static void format_timestamp(char *buffer, size_t size) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm *tm_info = localtime(&tv.tv_sec);
    char timestamp[64];

    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    snprintf(buffer, size, "%s.%06ld", timestamp, tv.tv_usec);
}

void log_message(const char *level, const char *fmt, ...) {
    char timestamp_us[80];
    format_timestamp(timestamp_us, sizeof(timestamp_us));

    const char *log_path = metrics_log_file_path[0] ? metrics_log_file_path : METRICS_LOG_FILE;
    FILE *fp = fopen(log_path, "a");
    if (!fp) {
        return;
    }

    fprintf(fp, "[%s] [%s] ", timestamp_us, level);

    va_list args;
    va_start(args, fmt);
    vfprintf(fp, fmt, args);
    va_end(args);

    fputc('\n', fp);
    fclose(fp);
}

long long get_total_cpu_ms(int process_pid) {
    long long total_cpu_ms = 0;
    char stat_path[64];
    snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", process_pid);
    
    FILE *fstat = fopen(stat_path, "r");
    if (fstat == NULL) {
        return 0;
    }
    
    unsigned long utime = 0, stime = 0;
    // Parse /proc/[pid]/stat: fields 14 and 15 are utime and stime in clock ticks
    if (fscanf(fstat, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*lu %*lu %*lu %*lu %lu %lu",
               &utime, &stime) == 2) {
        // Get clock ticks per second
        long clk_tck = sysconf(_SC_CLK_TCK);
        if (clk_tck <= 0) clk_tck = 100;  // Default fallback
        
        // Convert clock ticks to milliseconds: (ticks / clk_tck) * 1000
        total_cpu_ms = ((utime + stime) * 1000LL) / clk_tck;
    }
    
    fclose(fstat);
    return total_cpu_ms;
}

long get_total_memory_kb(int process_pid) {
    long memory_kb = 0;
    char status_path[64];
    snprintf(status_path, sizeof(status_path), "/proc/%d/status", process_pid);

    FILE *fstatus = fopen(status_path, "r");
    if (fstatus == NULL) {
        return 0;
    }

    char line[256];
    while (fgets(line, sizeof(line), fstatus)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line + 6, "%ld", &memory_kb);
            break;
        }
    }
    fclose(fstatus);

    return memory_kb;
}

struct cpu_stat_snapshot {
    unsigned long long user;
    unsigned long long nice;
    unsigned long long system;
    unsigned long long idle;
    unsigned long long iowait;
    unsigned long long irq;
    unsigned long long softirq;
    unsigned long long steal;
    unsigned long long guest;
    unsigned long long guest_nice;
};

static int read_cpu_stat_snapshot(struct cpu_stat_snapshot *snapshot) {
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) {
        return 0;
    }

    char line[256];
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return 0;
    }

    int parsed = sscanf(line,
        "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
        &snapshot->user,
        &snapshot->nice,
        &snapshot->system,
        &snapshot->idle,
        &snapshot->iowait,
        &snapshot->irq,
        &snapshot->softirq,
        &snapshot->steal,
        &snapshot->guest,
        &snapshot->guest_nice);

    fclose(fp);
    return parsed >= 4;
}

static unsigned long long cpu_snapshot_total_ticks(const struct cpu_stat_snapshot *snapshot) {
    return snapshot->user + snapshot->nice + snapshot->system + snapshot->idle + snapshot->iowait +
           snapshot->irq + snapshot->softirq + snapshot->steal + snapshot->guest + snapshot->guest_nice;
}

static unsigned long long cpu_snapshot_active_ticks(const struct cpu_stat_snapshot *snapshot) {
    return snapshot->user + snapshot->nice + snapshot->system + snapshot->irq + snapshot->softirq + snapshot->steal +
           snapshot->guest + snapshot->guest_nice;
}

static long read_memory_usage_kb(void) {
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) {
        return 0;
    }

    long mem_total_kb = 0;
    long mem_available_kb = 0;
    long mem_free_kb = 0;
    long buffers_kb = 0;
    long cached_kb = 0;

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "MemTotal:", 9) == 0) {
            sscanf(line + 9, "%ld", &mem_total_kb);
        } else if (strncmp(line, "MemAvailable:", 13) == 0) {
            sscanf(line + 13, "%ld", &mem_available_kb);
        } else if (strncmp(line, "MemFree:", 8) == 0) {
            sscanf(line + 8, "%ld", &mem_free_kb);
        } else if (strncmp(line, "Buffers:", 8) == 0) {
            sscanf(line + 8, "%ld", &buffers_kb);
        } else if (strncmp(line, "Cached:", 7) == 0) {
            sscanf(line + 7, "%ld", &cached_kb);
        }
    }
    fclose(fp);

    if (mem_total_kb > 0 && mem_available_kb > 0) {
        return mem_total_kb - mem_available_kb;
    }
    if (mem_total_kb > 0) {
        return mem_total_kb - (mem_free_kb + buffers_kb + cached_kb);
    }
    return 0;
}

static float read_cpu_temperature_c(void) {
    DIR *dir = opendir("/sys/class/thermal");
    if (!dir) {
        return 0.0f;
    }

    struct dirent *entry;
    float temperature_c = 0.0f;

    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "thermal_zone", 12) != 0) {
            continue;
        }

        char path[256];
        snprintf(path, sizeof(path), "/sys/class/thermal/%s/temp", entry->d_name);

        FILE *fp = fopen(path, "r");
        if (!fp) {
            continue;
        }

        int temp_milli = 0;
        if (fscanf(fp, "%d", &temp_milli) == 1 && temp_milli > 0) {
            temperature_c = (float)temp_milli / 1000.0f;
        }
        fclose(fp);

        if (temperature_c > 0.0f) {
            closedir(dir);
            return temperature_c;
        }
    }

    closedir(dir);
    return 0.0f;
}

static long read_cpu_frequency_khz(void) {
    static const char *paths[] = {
        "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq",
        "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_cur_freq",
        "/sys/devices/system/cpu/cpu/cpufreq/scaling_cur_freq"
    };

    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        FILE *fp = fopen(paths[i], "r");
        if (!fp) {
            continue;
        }

        long freq_khz = 0;
        if (fscanf(fp, "%ld", &freq_khz) == 1 && freq_khz > 0) {
            fclose(fp);
            return freq_khz;
        }
        fclose(fp);
    }

    FILE *fp = fopen("/proc/cpuinfo", "r");
    if (!fp) {
        return 0;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "cpu MHz", 8) == 0) {
            double cpu_mhz = 0.0;
            if (sscanf(line, "cpu MHz : %lf", &cpu_mhz) == 1) {
                fclose(fp);
                return (long)(cpu_mhz * 1000.0);
            }
        }
    }

    fclose(fp);
    return 0;
}

void sample_metrics(int process_pid, int sample_interval_ms, const char *output_path) {
    FILE *fp = fopen(output_path, "w");
    if (fp == NULL) {
        log_message("ERROR", "Unable to open output file: %s", output_path);
        return;
    }

    fprintf(fp, "timestamp,cpu_percent,cpu_total_ms,classifier_memory_kb\n");
    fflush(fp);

    char stat_path[64];
    snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", process_pid);
    long memory_usage = 0;
    long long actual_cpu_ms = 0;
    long long last_cpu_ms = 0;
    float cpu_usage = 0.0f;

    struct timespec time_last;
    struct timespec time_captured;
    struct timespec time_now;

    int processed = 1U;

    last_cpu_ms = get_total_cpu_ms(process_pid);

    clock_gettime(CLOCK_REALTIME, &time_last);
    time_captured = time_last;

    while (1) {
        struct stat st;
        if (stat(stat_path, &st) != 0) {
            log_message("INFO", "Process ended, stopping sampler");
            break;
        }

        clock_gettime(CLOCK_REALTIME, &time_now);

        long long delta_time = (time_now.tv_sec - time_last.tv_sec) * 1000000000LL +
                               (time_now.tv_nsec - time_last.tv_nsec);

        if(delta_time > sample_interval_ms * 1000000LL) {
            actual_cpu_ms = get_total_cpu_ms(process_pid);
            memory_usage = get_total_memory_kb(process_pid);

            clock_gettime(CLOCK_REALTIME, &time_captured);
            processed = 0U;
        }

        if(processed == 0U) {
            long long sample_time = (time_captured.tv_sec - time_last.tv_sec) * 1000000000LL +
                               (time_captured.tv_nsec - time_last.tv_nsec);
            long long delta_cpu_ms = actual_cpu_ms - last_cpu_ms;
            cpu_usage = (float)delta_cpu_ms / (float)(sample_time/1000000LL) * 100.0f;
            last_cpu_ms = actual_cpu_ms;
            time_last = time_captured;
            processed = 1U;

            struct tm *tm_info = localtime(&time_captured.tv_sec);
            char timestamp[32];
            strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
            long milliseconds = time_captured.tv_nsec / 1000000;

            fprintf(fp, "%s.%03ld,%.2f,%lld,%ld\n", timestamp, milliseconds, cpu_usage, actual_cpu_ms, memory_usage);
            fflush(fp);
        }

        usleep(1000);
    }

    fclose(fp);
    log_message("INFO", "Sampling complete");
}

void sample_system_metrics(int sample_interval_ms, const char *output_path) {
    FILE *fp = fopen(output_path, "w");
    if (fp == NULL) {
        log_message("ERROR", "Unable to open output file: %s", output_path);
        return;
    }

    fprintf(fp, "timestamp,cpu_time_ms,cpu_percentage,memory_usage_kb,cpu_temperature_c,cpu_frequency_khz\n");
    fflush(fp);

    struct cpu_stat_snapshot last_cpu = {0};
    struct cpu_stat_snapshot current_cpu = {0};
    struct timespec time_last;
    struct timespec time_now;
    long cpu_time_ms = 0;
    float cpu_percent = 0.0f;
    long memory_usage_kb = 0;
    float cpu_temperature_c = 0.0f;
    long cpu_frequency_khz = 0;

    if (!read_cpu_stat_snapshot(&last_cpu)) {
        log_message("ERROR", "Unable to read initial system CPU stats");
        fclose(fp);
        return;
    }

    clock_gettime(CLOCK_REALTIME, &time_last);

    while (1) {
        clock_gettime(CLOCK_REALTIME, &time_now);
        long long delta_time_ns = (time_now.tv_sec - time_last.tv_sec) * 1000000000LL +
                                  (time_now.tv_nsec - time_last.tv_nsec);

        if (delta_time_ns >= sample_interval_ms * 1000000LL) {
            if (!read_cpu_stat_snapshot(&current_cpu)) {
                log_message("ERROR", "Unable to read CPU stats while sampling system metrics");
                break;
            }

            unsigned long long delta_total_ticks = cpu_snapshot_total_ticks(&current_cpu) - cpu_snapshot_total_ticks(&last_cpu);
            unsigned long long delta_active_ticks = cpu_snapshot_active_ticks(&current_cpu) - cpu_snapshot_active_ticks(&last_cpu);
            long clk_tck = sysconf(_SC_CLK_TCK);
            if (clk_tck <= 0) {
                clk_tck = 100;
            }

            if (delta_total_ticks > 0) {
                cpu_percent = ((float)delta_active_ticks / (float)delta_total_ticks) * 100.0f;
            } else {
                cpu_percent = 0.0f;
            }

            cpu_time_ms = (long)((cpu_snapshot_total_ticks(&current_cpu) * 1000LL) / clk_tck);
            memory_usage_kb = read_memory_usage_kb();
            cpu_temperature_c = read_cpu_temperature_c();
            cpu_frequency_khz = read_cpu_frequency_khz();

            struct tm *tm_info = localtime(&time_now.tv_sec);
            char timestamp[32];
            strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
            long milliseconds = time_now.tv_nsec / 1000000;

            fprintf(fp, "%s.%03ld,%.2f,%.2f,%ld,%.2f,%ld\n",
                    timestamp,
                    milliseconds,
                    (float)cpu_time_ms,
                    cpu_percent,
                    memory_usage_kb,
                    cpu_temperature_c,
                    cpu_frequency_khz);
            fflush(fp);

            last_cpu = current_cpu;
            time_last = time_now;
        }

        usleep(1000);
    }

    fclose(fp);
    log_message("INFO", "System sampling complete");
}

int main(int argc, char *argv[]) {
    if (argc != 5) {
        log_message("ERROR", "Usage: %s <process_pid> <SAMPLE_INTERVAL_MS> <process_name> <output_path>\n", argv[0]);
        log_message("ERROR", "Example: %s 1234 500 metrics \n", argv[0]);
        return EXIT_FAILURE;
    }

    int process_pid = atoi(argv[1]);
    int sample_interval_ms = atoi(argv[2]);
    const char *process_name = argv[3];
    const char *output_path = argv[4];
    snprintf(metrics_csv_file_path, sizeof(metrics_csv_file_path), "%s/%s_metrics.csv", output_path, process_name);
    snprintf(metrics_log_file_path, sizeof(metrics_log_file_path), "%s/%s_%s", output_path, process_name, METRICS_LOG_FILE);

    log_message("INFO", "Metrics Sampler Starting");
    log_message("INFO", "Process name: %s", process_name);
    log_message("INFO", "Process PID: %d", process_pid);
    log_message("INFO", "Sample interval: %d miliseconds", sample_interval_ms);
    log_message("INFO", "Metrics file: %s", metrics_csv_file_path);
    log_message("INFO", "Log file: %s", metrics_log_file_path);

    if (sample_interval_ms <= 0) {
        log_message("ERROR", "Invalid sample interval.");
        return EXIT_FAILURE;
    }

    if (process_pid <= 0) {
        log_message("INFO", "System-wide monitoring enabled");
        sample_system_metrics(sample_interval_ms, metrics_csv_file_path);
    }
    else
    {
        sample_metrics(process_pid, sample_interval_ms, metrics_csv_file_path);
    }

    return EXIT_SUCCESS;
}