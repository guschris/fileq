#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

#define DEFAULT_TASK_DIR "tasks"
#define COMPLETE_DIR "complete"
#define EVENT_SIZE (sizeof(struct inotify_event))
#define EVENT_BUF_LEN (1024 * (EVENT_SIZE + 16))

void execute_task(const char *filepath, const char *filename, int fd) {
    time_t start_time, end_time;
    double elapsed_time;
    int status;

    fprintf(stderr, "Task '%s' starting...\n", filename);
    start_time = time(NULL);

    FILE *file = fdopen(fd, "r");
    if (file == NULL) {
        perror("fdopen");
        fprintf(stderr, "Task '%s' failed to open file.\n", filename);
        return;
    }

    // Determine the size of the command
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    // do NOT close the file here, it will be closed by the caller

    if (file_size <= 0) {
        fprintf(stderr, "Task '%s' failed to determine command size.\n", filename);
        return;
    }

    char *command = malloc(file_size + 1);
    if (command == NULL) {
        perror("malloc");
        fprintf(stderr, "Task '%s' failed to allocate memory for command.\n", filename);
        return;
    }

    if (fgets(command, file_size + 1, file) == NULL) {
        free(command);
        fprintf(stderr, "Task '%s' failed to read command.\n", filename);
        return;
    }
    command[strcspn(command, "\n")] = 0;

    pid_t pid = fork();
    if (pid == 0) {
        // child process started
        execl("/bin/sh", "/bin/sh", "-c", command, NULL);
        perror("execl");
        exit(1);
    } else if (pid > 0) {
        // parent process waiting for child to finish
        waitpid(pid, &status, 0);

        end_time = time(NULL);
        elapsed_time = difftime(end_time, start_time);

        char completed_filepath[PATH_MAX];
        snprintf(completed_filepath, sizeof(completed_filepath), "%s/%s", COMPLETE_DIR, filename);

        if (rename(filepath, completed_filepath) != 0) {
            perror("rename");
            fprintf(stderr, "Task '%s' rename from %s to %s failed.\n", filename, filepath, completed_filepath);
        } else if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            fprintf(stderr, "Task '%s' completed in %.2f seconds. moved task to %s\n", filename, elapsed_time, completed_filepath);
        } else {
            fprintf(stderr, "Task '%s' failed (exit code %d) in %.2f seconds.\n", filename, WEXITSTATUS(status), elapsed_time);
        }
    } else {
        perror("fork");
        fprintf(stderr, "Task '%s' fork failed.\n", filename);
    }
    free(command);
}

int run_next_task(char *task_dir) {
    DIR *dir;
    struct dirent *entry;
    struct dirent **namelist;
    int n;

    n = scandir(task_dir, &namelist, NULL, alphasort);
    if (n < 0) {
        perror("scandir");
        return 0;
    }

    for (int i = 0; i < n; i++) {
        entry = namelist[i];
        if (entry->d_type == DT_REG) {
            char filepath[PATH_MAX];
            snprintf(filepath, sizeof(filepath), "%s/%s", task_dir, entry->d_name);

            // lock the file without waiting
            int fd = open(filepath, O_RDWR);
            if (fd == -1) {
                // don't print error, another process may have already processed the file
                free(entry);
                continue;
            }
            struct flock fl = {F_WRLCK, SEEK_SET, 0, 0, 0};
            if (fcntl(fd, F_SETLK, &fl) == -1) {
                // don't print error, another process may have the file locked already
                close(fd);
                free(entry);
                continue;
            }

            execute_task(filepath, entry->d_name, fd);
            close(fd);
            free(entry);
            free(namelist);
            return 1;
        }
        free(entry);
    }
    free(namelist);
    return 0;
}

void run_all_tasks(char *task_dir) {
    while (run_next_task(task_dir)) {
        // Loop until no more tasks
    }
}

void watch_for_changes(char *task_dir) {
    int inotify_fd = -1, watch_fd = -1;
    char buffer[EVENT_BUF_LEN];

    inotify_fd = inotify_init();
    if (inotify_fd < 0) {
        perror("inotify_init");
        return;
    }

    watch_fd = inotify_add_watch(inotify_fd, task_dir, IN_CREATE | IN_DELETE);
    if (watch_fd < 0) {
        perror("inotify_add_watch");
        close(inotify_fd);
        return;
    }

    // Scan again immediately after inotify_add_watch in case there are new tasks we missed
    run_all_tasks(task_dir);

    while (1) {
        int length = read(inotify_fd, buffer, EVENT_BUF_LEN);
        if (length < 0) {
            perror("read");
            break;
        }

        int i = 0;
        while (i < length) {
            struct inotify_event *event = (struct inotify_event *)&buffer[i];
            if (event->len) {
                if (event->mask & (IN_CREATE | IN_DELETE)) { // Rescan on create or delete
                    run_all_tasks(task_dir);
                }
            }
            i += EVENT_SIZE + event->len;
        }
    }

    inotify_rm_watch(inotify_fd, watch_fd);
    close(inotify_fd);
}

int main(int argc, char *argv[]) {
    int inotify_fd = -1, watch_fd = -1;
    char buffer[EVENT_BUF_LEN];
    int watch_mode = 0;
    char *task_dir = DEFAULT_TASK_DIR;
    int num_instances = 1; // Default number of instances

    // Parse command-line options
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--watch") == 0) {
            watch_mode = 1;
        } else if (strncmp(argv[i], "-N=", 3) == 0) {
            num_instances = atoi(argv[i] + 3);
            if (num_instances <= 0) {
                fprintf(stderr, "Invalid number of instances. Using default value of 1.\n");
                num_instances = 1;
            }
        } else {
            task_dir = argv[i];
        }
    }

    // Create the "complete" directory if it doesn't exist
    struct stat st = {0};
    if (stat(COMPLETE_DIR, &st) == -1) {
        mkdir(COMPLETE_DIR, 0700);
    }

    // Run multiple instances
    if (num_instances == 1) {
        run_all_tasks(task_dir);
        if (watch_mode) {
            watch_for_changes(task_dir);
        }
        return 0;
    }

    // Fork multiple instances
    fprintf(stderr, "Running tasks in '%s' with %d instance(s)...\n", task_dir, num_instances);

    for (int i = 0; i < num_instances; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            // Child process
            fprintf(stderr, "Instance %d starting...\n", i + 1);
            // Resume existing tasks
            run_all_tasks(task_dir);
            if (watch_mode) {
                watch_for_changes(task_dir);
            }        
            fprintf(stderr, "Instance %d finished.\n", i + 1);
            exit(0); // Exit child process after completing tasks
        } else if (pid < 0) {
            perror("fork");
            fprintf(stderr, "Failed to create instance %d.\n", i + 1);
        }
    }

    // Parent process waits for all child processes to complete
    for (int i = 0; i < num_instances; i++) {
        int status;
        wait(&status);
    }

    return 0;
}