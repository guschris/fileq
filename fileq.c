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

void execute_task(const char *filepath, const char *filename) {
    time_t start_time, end_time;
    double elapsed_time;
    int status;

    fprintf(stderr, "Task '%s' starting...\n", filename);
    start_time = time(NULL);

    int fd = open(filepath, O_RDWR);
    if (fd == -1) {
        perror("open");
        fprintf(stderr, "Task '%s' failed to open.\n", filename);
        return;
    }
    struct flock fl = {F_WRLCK, SEEK_SET, 0, 0, 0};
    if (fcntl(fd, F_SETLK, &fl) == -1) {
        perror("fcntl");
        close(fd);
        fprintf(stderr, "Task '%s' failed to lock.\n", filename);
        return;
    }

    FILE *file = fopen(filepath, "r");
    if (file == NULL) {
        perror("fopen");
        close(fd);
        fprintf(stderr, "Task '%s' failed to open file.\n", filename);
        return;
    }

    char command[1024];
    if (fgets(command, sizeof(command), file) == NULL) {
        fclose(file);
        close(fd);
        fprintf(stderr, "Task '%s' failed to read command.\n", filename);
        return;
    }
    fclose(file);
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
            fprintf(stderr, "Task '%s' rename failed.\n", filename);
        } else {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                fprintf(stderr, "Task '%s' completed in %.2f seconds. moved task to %s\n", filename, elapsed_time, completed_filepath);
            } else {
                fprintf(stderr, "Task '%s' failed (exit code %d) in %.2f seconds.\n", filename, WEXITSTATUS(status), elapsed_time);
            }
        }
    } else {
        perror("fork");
        fprintf(stderr, "Task '%s' fork failed.\n", filename);
        // Move the task file if fork fails
        char completed_filepath[PATH_MAX];
        snprintf(completed_filepath, sizeof(completed_filepath), "%s/%s", COMPLETE_DIR, filename);
        rename(filepath, completed_filepath);
    }
    close(fd);
}

void resume_existing_tasks(char *task_dir) {
    DIR *dir;
    struct dirent *entry;
    struct dirent *first_entry = NULL; // Store the first entry

    while (1) { // Loop until no more tasks
        dir = opendir(task_dir);
        if (dir == NULL) {
            perror("opendir");
            return;
        }

        first_entry = NULL; // Reset for each iteration

        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_type == DT_REG) {
                if (first_entry == NULL) {
                    first_entry = malloc(sizeof(struct dirent));
                    memcpy(first_entry, entry, sizeof(struct dirent));
                } else if (strcmp(entry->d_name, first_entry->d_name) < 0) {
                    // Found a filename that comes alphabetically before the current first_entry
                    free(first_entry);
                    first_entry = malloc(sizeof(struct dirent));
                    memcpy(first_entry, entry, sizeof(struct dirent));
                }
            }
        }
        closedir(dir);

        if (first_entry != NULL) {
            char filepath[PATH_MAX];
            snprintf(filepath, sizeof(filepath), "%s/%s", task_dir, first_entry->d_name);
            execute_task(filepath, first_entry->d_name);
            free(first_entry);
        } else {
            // No more tasks found
            break;
        }
    }
}

int main(int argc, char *argv[]) {
    int inotify_fd = -1, watch_fd = -1;
    char buffer[EVENT_BUF_LEN];
    int watch_mode = 0;
    char *task_dir = DEFAULT_TASK_DIR;

    // Parse command-line options
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--watch") == 0) {
            watch_mode = 1;
        } else {
            task_dir = argv[i];
        }
    }

    fprintf(stderr, "Running tasks in '%s'...\n", task_dir);

    // Create the "complete" directory if it doesn't exist
    struct stat st = {0};
    if (stat(COMPLETE_DIR, &st) == -1) {
        mkdir(COMPLETE_DIR, 0700);
    }

    // Resume existing tasks
    resume_existing_tasks(task_dir);

    if (watch_mode) {
        inotify_fd = inotify_init();
        if (inotify_fd < 0) {
            perror("inotify_init");
            return 1;
        }

        watch_fd = inotify_add_watch(inotify_fd, task_dir, IN_CREATE | IN_DELETE);
        if (watch_fd < 0) {
            perror("inotify_add_watch");
            close(inotify_fd);
            return 1;
        }

        // Scan again immediately after inotify_add_watch
        resume_existing_tasks(task_dir);

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
                    if (event->mask & IN_CREATE) {
                        if (!(event->mask & IN_ISDIR)) {
                            char filepath[PATH_MAX];
                            snprintf(filepath, sizeof(filepath), "%s/%s", task_dir, event->name);
                            execute_task(filepath, event->name);
                        }
                    } else if (event->mask & IN_DELETE) {
                        // ... (handle delete events if needed) ...
                    }
                }
                i += EVENT_SIZE + event->len;
            }
        }

        inotify_rm_watch(inotify_fd, watch_fd);
        close(inotify_fd);
    }

    return 0;
}