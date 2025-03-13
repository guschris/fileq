# fileq
This is a small (18K) file-based task queue implemented in C. It monitors a directory for task files, executes the commands specified in those files, and moves the completed or failed task files to a designated "complete" directory.  

To make task running "robust", use [autorestart](https://github.com/guschris/autorestart) within your tasks.

## Features

* **File-Based Tasks:** Tasks are represented by files in a specified directory.
* **Ordered Execution:** Tasks are executed in alphabetical order based on their filenames.
* **Persistence:** Tasks survive system reboots.
* **Logging:** Task start, completion, and error messages are logged to standard error (`stderr`).
* **Task Completion/Failure Handling:** Completed and failed tasks are moved to a "complete" subdirectory.
* **`inotify` Monitoring (Optional):** Continuously monitors the task directory for new tasks.
* **Custom Task Directory:** The task directory can be specified as a command-line argument.
* **Concurrent task execution:** run multiple instances of the program on the same task directory to execute multiple tasks at the same time 

## Compilation

To compile the program, use the following command:
```sh
./build
```

## Usage

```sh
./fileq [-N=x] [--watch] [task_directory] 
```
### Options

* `-N=x` (optional): run multiple instances to process tasks concurrently, where x is the number of instances.
* `task_directory` (optional): Specifies the directory to monitor for task files. If not provided, the default directory "tasks" is used.
* `--watch` (optional): Enables continuous monitoring of the task directory using inotify. If this option is not provided, the program will process existing tasks and then exit.

### Task File Format
* Each task is represented by a regular file in the task directory.
* The filename is used for alphabetical sorting of the tasks.
* The content of the file should be a single line containing the command to execute.  To make task running "robust", use [autorestart](https://github.com/guschris/autorestart) within your tasks.

### Output
The program outputs log messages to standard error (stderr). These messages indicate when tasks start, complete, or fail, along with the elapsed time for each task.

### Completed Tasks
Completed and failed task files are moved to the "complete" subdirectory within the task directory.

## Notes

* The program uses file locking to prevent concurrent access to task files.
* Tasks are executed in alphabetical order based on their filenames.
* Error messages are logged to stderr.
* The program resumes existing tasks on startup.
* The program uses inotify to detect new files.