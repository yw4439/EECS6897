#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <bpf/bpf.h>
#include <sys/resource.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>

#define TASK_MAP_PATH "/sys/fs/bpf/task_map"
#define CHECK_INTERVAL 5 // Adjust this to the desired interval (in seconds)

struct task_info {
    uint32_t pid;             // Task PID
    bool time_sensitive;      // Whether the task is latency-sensitive
    uint64_t max_latency;     // Maximum allowable latency in nanoseconds
    uint64_t start_time;      // Start time for latency measurement
    uint64_t end_time;        // End time for latency measurement
    uint64_t priority_class;  // Priority class (e.g., 1 to 5)
};


// Function to pause lower-priority tasks using SIGSTOP
void pause_lower_priority_task(uint32_t pid) {
    printf("Pausing task PID=%u using SIGSTOP...\n", pid);
    if (kill(pid, SIGSTOP) != 0) {
        perror("Failed to pause task");
    }
}

// Function to resume paused tasks using SIGCONT
void resume_lower_priority_task(uint32_t pid) {
    printf("Resuming task PID=%u using SIGCONT...\n", pid);
    if (kill(pid, SIGCONT) != 0) {
        perror("Failed to resume task");
    }
}

void check_and_prioritize_tasks() {
    int map_fd, key = 0, next_key;
    struct task_info task;
    uint64_t current_time;

    // Open the task_map
    map_fd = bpf_obj_get(TASK_MAP_PATH);
    if (map_fd < 0) {
        perror("Failed to open task_map");
        exit(1);
    }

    void throttle_lower_priority_tasks(uint32_t pid) {
        struct timespec ts;
        ts.tv_sec = 5;    // Sleep for 5 seconds
        ts.tv_nsec = 0;   // No additional nanoseconds

        printf("Throttling task PID=%u with nanosleep for 5 seconds...\n", pid);
        nanosleep(&ts, NULL);
    } 

    bool indicator; //check if any task go overtime

    while (1) {
        indicator = 0; //set indicator to 0 every loop
        // Get the current time for real-time latency calculation
        current_time = (uint64_t)time(NULL) * 1000000000ULL; // Convert to nanoseconds

        // Iterate through all tasks in the task_map
        while (bpf_map_get_next_key(map_fd, &key, &next_key) == 0) {
            if (bpf_map_lookup_elem(map_fd, &next_key, &task) == 0) {
                // Check if the task is still valid
                if (kill(task.pid, 0) != 0) {
                    continue; // Do not delete for testing purposes
                }

                // Calculate ongoing latency (real-time monitoring)
                uint64_t latency = current_time - task.start_time;

                // Pause lower-priority classes if latency exceeds the maximum
                if (task.time_sensitive && latency > task.max_latency) {
                    indicator = 1;
                    printf("Task PID=%u exceeded max latency (%lu ns > %lu ns), prioritizing...\n",
                           task.pid, latency, task.max_latency);

                    // Pause all lower-priority processes
                    for (int i = task.priority_class + 1; i <= 5; i++) {
                        uint32_t lower_key = 0, lower_next_key;
                        struct task_info lower_task;

                        while (bpf_map_get_next_key(map_fd, &lower_key, &lower_next_key) == 0) {
                            if (bpf_map_lookup_elem(map_fd, &lower_next_key, &lower_task) == 0) {
                                if (lower_task.priority_class == i && kill(lower_task.pid, 0) == 0) {
                                    printf("Throttling lower-priority task PID=%u (Priority=%lu)\n",
                                       lower_task.pid, lower_task.priority_class);
                                    throttle_lower_priority_tasks(lower_task.pid);
                                }
                            }
                        lower_key = lower_next_key;
                        }
                    }

                    // Assign the highest priority
                    if (setpriority(PRIO_PROCESS, task.pid, -20) == 0) {
                        printf("Successfully prioritized task PID=%u\n", task.pid);
                    } else {
                        perror("Failed to set priority");
                    }
                } else if (task.time_sensitive) {
                    // Adjust priority based on latency thresholds
                    if (latency >= (task.max_latency * 0.75)) {
                        printf("Task PID=%u approaching max latency, increasing priority\n", task.pid);
                        setpriority(PRIO_PROCESS, task.pid, -10);
                    } else if (latency >= (task.max_latency * 0.5)) {
                        printf("Task PID=%u halfway to max latency, increasing priority slightly\n", task.pid);
                        setpriority(PRIO_PROCESS, task.pid, 0);
                    }
                }
            }
            if (!indicator){
                printf("Resuming all paused tasks...\n");
                for (int i = 1; i <= 5; i++) {
                    uint32_t lower_key = 0, lower_next_key;
                    struct task_info lower_task;

                    while (bpf_map_get_next_key(map_fd, &lower_key, &lower_next_key) == 0) {
                        if (bpf_map_lookup_elem(map_fd, &lower_next_key, &lower_task) == 0) {
                            if (lower_task.priority_class == i && kill(lower_task.pid, 0) == 0) {
                                resume_lower_priority_task(lower_task.pid);
                            }
                        }
                        lower_key = lower_next_key;
                    }
                }

            key = next_key; // Move to the next key
            }

        // Sleep for the specified interval
        sleep(CHECK_INTERVAL);
        }

    close(map_fd);
    }
}

int main() {
    printf("Starting task monitoring and prioritization...\n");
    check_and_prioritize_tasks();
    return 0;
}
