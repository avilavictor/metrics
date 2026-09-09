#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdarg.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <signal.h>

#define METRICS_LOG_FILE "metrics_debug.log"
#define MAX_PROCESSES 64
#define MAX_NAME_LEN 128

static char metrics_log_file_path[512];
static long g_clk_tck = 0;
static volatile sig_atomic_t g_stop_requested = 0;

typedef struct {
    int pid;
    char name[MAX_NAME_LEN];
    char csv_path[512];
    FILE *fp;
    long long last_cpu_ms;
    int alive;
} monitored_process_t;

static void handle_signal(int sig) {
    (void)sig;
    g_stop_requested = 1;
}

static long get_clk_tck(void) {
    if (g_clk_tck <= 0) {
        g_clk_tck = sysconf(_SC_CLK_TCK);
        if (g_clk_tck <= 0) {
            g_clk_tck = 100;
        }
    }
    return g_clk_tck;
}

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

// Reads /proc/[pid]/stat once. Returns 1 on success (process alive and parsed),
// 0 if the process no longer exists or could not be parsed.
static int get_process_cpu_ms(int process_pid, long long *out_cpu_ms) {
    char stat_path[64];
    snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", process_pid);

    FILE *fstat = fopen(stat_path, "r");
    if (fstat == NULL) {
        return 0; // process ended (or inaccessible)
    }

    unsigned long utime = 0, stime = 0;
    // Parse /proc/[pid]/stat: fields 14 and 15 are utime and stime in clock ticks
    int parsed = fscanf(fstat, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*lu %*lu %*lu %*lu %lu %lu",
                         &utime, &stime);
    fclose(fstat);

    if (parsed != 2) {
        return 0;
    }

    *out_cpu_ms = ((long long)(utime + stime) * 1000LL) / get_clk_tck();
    return 1;
}

static long get_process_memory_kb(int process_pid) {
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

static void write_timestamp(FILE *fp, struct timespec *ts) {
    struct tm *tm_info = localtime(&ts->tv_sec);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    long milliseconds = ts->tv_nsec / 1000000;
    fprintf(fp, "%s.%03ld", timestamp, milliseconds);
}

// Advances 'ts' by interval_ms, normalizing tv_nsec overflow.
static void advance_timespec(struct timespec *ts, int interval_ms) {
    ts->tv_nsec += (long)interval_ms * 1000000L;
    while (ts->tv_nsec >= 1000000000L) {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec += 1;
    }
}

// Consolidated loop: monitors N processes (by PID) AND/OR system-wide metrics
// using a single clock_nanosleep per cycle, instead of one process per PID.
// This avoids N independent wakeup sources competing for CPU/scheduler time.
static void run_monitor(monitored_process_t *procs, int proc_count,
                         int system_mode, int sample_interval_ms,
                         FILE *system_fp) {
    struct cpu_stat_snapshot last_cpu = {0};
    struct cpu_stat_snapshot current_cpu;

    if (system_mode) {
        if (!read_cpu_stat_snapshot(&last_cpu)) {
            log_message("ERROR", "Unable to read initial system CPU stats");
            system_mode = 0;
        }
    }

    for (int i = 0; i < proc_count; i++) {
        long long cpu_ms = 0;
        get_process_cpu_ms(procs[i].pid, &cpu_ms);
        procs[i].last_cpu_ms = cpu_ms;
        procs[i].alive = 1;
    }

    int active_count = proc_count;

    struct timespec next_wake;
    struct timespec prev_wake;
    clock_gettime(CLOCK_MONOTONIC, &next_wake);
    prev_wake = next_wake;

    while (!g_stop_requested && (active_count > 0 || system_mode)) {
        advance_timespec(&next_wake, sample_interval_ms);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_wake, NULL);

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long long elapsed_ns = (now.tv_sec - prev_wake.tv_sec) * 1000000000LL +
                                (now.tv_nsec - prev_wake.tv_nsec);
        float elapsed_ms = (float)elapsed_ns / 1000000.0f;
        prev_wake = now;

        if (system_mode) {
            if (!read_cpu_stat_snapshot(&current_cpu)) {
                log_message("ERROR", "Unable to read CPU stats while sampling system metrics");
                system_mode = 0;
            } else {
                unsigned long long delta_active_ticks =
                    cpu_snapshot_active_ticks(&current_cpu) - cpu_snapshot_active_ticks(&last_cpu);
                long long delta_active_ms = (delta_active_ticks * 1000LL) / get_clk_tck();

                float cpu_percent = elapsed_ms > 0.0f
                    ? ((float)delta_active_ms / elapsed_ms) * 100.0f
                    : 0.0f;
                long cpu_time_ms = (long)((cpu_snapshot_total_ticks(&current_cpu) * 1000LL) / get_clk_tck());
                long memory_usage_kb = read_memory_usage_kb();
                float cpu_temperature_c = read_cpu_temperature_c();
                long cpu_frequency_khz = read_cpu_frequency_khz();

                write_timestamp(system_fp, &now);
                fprintf(system_fp, ",%.2f,%.2f,%ld,%.2f,%ld\n",
                        (float)cpu_time_ms, cpu_percent, memory_usage_kb,
                        cpu_temperature_c, cpu_frequency_khz);
                fflush(system_fp);

                last_cpu = current_cpu;
            }
        }

        for (int i = 0; i < proc_count; i++) {
            if (!procs[i].alive) {
                continue;
            }

            long long cpu_ms = 0;
            if (!get_process_cpu_ms(procs[i].pid, &cpu_ms)) {
                log_message("INFO", "Process %d (%s) ended, stopping its sampler", procs[i].pid, procs[i].name);
                procs[i].alive = 0;
                active_count--;
                fclose(procs[i].fp);
                procs[i].fp = NULL;
                continue;
            }

            long memory_kb = get_process_memory_kb(procs[i].pid);
            long long delta_cpu_ms = cpu_ms - procs[i].last_cpu_ms;
            float cpu_percent = elapsed_ms > 0.0f
                ? ((float)delta_cpu_ms / elapsed_ms) * 100.0f
                : 0.0f;
            procs[i].last_cpu_ms = cpu_ms;

            write_timestamp(procs[i].fp, &now);
            fprintf(procs[i].fp, ",%.2f,%lld,%ld\n", cpu_percent, cpu_ms, memory_kb);
            fflush(procs[i].fp);
        }
    }

    for (int i = 0; i < proc_count; i++) {
        if (procs[i].fp) {
            fclose(procs[i].fp);
        }
    }
    log_message("INFO", "Sampling complete");
}

static int parse_int_list(const char *arg, int *out, int max_count) {
    char buffer[1024];
    strncpy(buffer, arg, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

    int count = 0;
    char *saveptr = NULL;
    char *token = strtok_r(buffer, ",", &saveptr);
    while (token != NULL) {
        if (count >= max_count) {
            return -1;
        }
        out[count++] = atoi(token);
        token = strtok_r(NULL, ",", &saveptr);
    }
    return count;
}

// Splits a comma-separated list of names into 'out' (array of fixed-size buffers).
static int parse_name_list(const char *arg, char out[][MAX_NAME_LEN], int max_count) {
    char buffer[1024];
    strncpy(buffer, arg, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

    int count = 0;
    char *saveptr = NULL;
    char *token = strtok_r(buffer, ",", &saveptr);
    while (token != NULL) {
        if (count >= max_count) {
            return -1;
        }
        strncpy(out[count], token, MAX_NAME_LEN - 1);
        out[count][MAX_NAME_LEN - 1] = '\0';
        count++;
        token = strtok_r(NULL, ",", &saveptr);
    }
    return count;
}

static void print_usage(const char *prog) {
    log_message("ERROR", "Usage: %s <pid_list> <SAMPLE_INTERVAL_MS> <name_list> <output_path>", prog);
    log_message("ERROR", "  pid_list  : comma-separated PIDs (e.g. 1234,5678,9012), or 0 for system-wide only");
    log_message("ERROR", "  name_list : comma-separated names matching pid_list (ignored when pid_list is 0)");
    log_message("ERROR", "Examples:");
    log_message("ERROR", "  %s 1234,5678,9012 1000 app1,app2,app3 /var/log/metrics", prog);
    log_message("ERROR", "  %s 0 1000 system /var/log/metrics", prog);
}

int main(int argc, char *argv[]) {
    if (argc != 5) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    const char *pid_arg = argv[1];
    int sample_interval_ms = atoi(argv[2]);
    const char *name_arg = argv[3];
    const char *output_path = argv[4];

    snprintf(metrics_log_file_path, sizeof(metrics_log_file_path), "%s/%s", output_path, METRICS_LOG_FILE);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    log_message("INFO", "Metrics Sampler Starting");
    log_message("INFO", "Sample interval: %d milliseconds", sample_interval_ms);
    log_message("INFO", "Log file: %s", metrics_log_file_path);

    if (sample_interval_ms <= 0) {
        log_message("ERROR", "Invalid sample interval.");
        return EXIT_FAILURE;
    }

    int pids[MAX_PROCESSES];
    int pid_count = parse_int_list(pid_arg, pids, MAX_PROCESSES);
    if (pid_count <= 0) {
        log_message("ERROR", "Invalid or empty pid_list (max %d PIDs).", MAX_PROCESSES);
        return EXIT_FAILURE;
    }

    if (pid_count == 1 && pids[0] <= 0) {
        char system_csv_path[600];
        snprintf(system_csv_path, sizeof(system_csv_path), "%s/system_metrics.csv", output_path);
        FILE *system_fp = fopen(system_csv_path, "w");
        if (!system_fp) {
            log_message("ERROR", "Unable to open output file: %s", system_csv_path);
            return EXIT_FAILURE;
        }
        fprintf(system_fp, "timestamp,cpu_time_ms,cpu_percentage,memory_usage_kb,cpu_temperature_c,cpu_frequency_khz\n");
        fflush(system_fp);

        log_message("INFO", "System-wide monitoring enabled");
        log_message("INFO", "Metrics file: %s", system_csv_path);

        run_monitor(NULL, 0, 1, sample_interval_ms, system_fp);
        return EXIT_SUCCESS;
    }

    char names[MAX_PROCESSES][MAX_NAME_LEN];
    int name_count = parse_name_list(name_arg, names, MAX_PROCESSES);
    if (name_count != pid_count) {
        log_message("ERROR", "pid_list has %d entries but name_list has %d; they must match 1:1.",
                     pid_count, name_count);
        return EXIT_FAILURE;
    }

    monitored_process_t procs[MAX_PROCESSES];
    memset(procs, 0, sizeof(procs));

    for (int i = 0; i < pid_count; i++) {
        if (pids[i] <= 0) {
            log_message("ERROR", "Invalid PID '%d' in pid_list (mixing 0 with real PIDs is not supported).", pids[i]);
            return EXIT_FAILURE;
        }
        procs[i].pid = pids[i];
        strncpy(procs[i].name, names[i], MAX_NAME_LEN - 1);
        snprintf(procs[i].csv_path, sizeof(procs[i].csv_path), "%s/%s_metrics.csv", output_path, procs[i].name);

        procs[i].fp = fopen(procs[i].csv_path, "w");
        if (!procs[i].fp) {
            log_message("ERROR", "Unable to open output file: %s", procs[i].csv_path);
            // close any files already opened before bailing out
            for (int j = 0; j < i; j++) {
                if (procs[j].fp) fclose(procs[j].fp);
            }
            return EXIT_FAILURE;
        }
        fprintf(procs[i].fp, "timestamp,cpu_percent,cpu_total_ms,classifier_memory_kb\n");
        fflush(procs[i].fp);

        log_message("INFO", "Monitoring PID %d (%s) -> %s", procs[i].pid, procs[i].name, procs[i].csv_path);
    }

    run_monitor(procs, pid_count, 0, sample_interval_ms, NULL);
    return EXIT_SUCCESS;
}