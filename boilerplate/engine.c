#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#ifdef __linux__
#include <sys/mount.h>
#endif

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    int exit_status;
    uint32_t payload_length;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct supervisor_ctx supervisor_ctx_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    int stop_requested;
    int monitor_registered;
    int producer_started;
    int producer_joined;
    int log_read_fd;
    pthread_t producer_thread;
    void *child_stack;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    supervisor_ctx_t *ctx;
    int client_fd;
} request_thread_arg_t;

typedef struct {
    supervisor_ctx_t *ctx;
    int read_fd;
    char container_id[CONTAINER_ID_LEN];
} producer_arg_t;

struct supervisor_ctx {
    int server_fd;
    int monitor_fd;
    int should_stop;
    char base_rootfs[PATH_MAX];
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    pthread_cond_t metadata_cv;
    container_record_t *containers;
};

static volatile sig_atomic_t supervisor_got_sigchld;
static volatile sig_atomic_t supervisor_should_stop;
static volatile sig_atomic_t client_forward_stop;
static char client_run_id[CONTAINER_ID_LEN];

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static ssize_t write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    size_t total = 0;

    while (total < len) {
        ssize_t rc = write(fd, p + total, len - total);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (rc == 0)
            break;
        total += (size_t)rc;
    }

    return (ssize_t)total;
}

static ssize_t read_all(int fd, void *buf, size_t len)
{
    char *p = buf;
    size_t total = 0;

    while (total < len) {
        ssize_t rc = read(fd, p + total, len - total);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (rc == 0)
            break;
        total += (size_t)rc;
    }

    return (ssize_t)total;
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int is_terminal_state(container_state_t state)
{
    return state == CONTAINER_STOPPED ||
           state == CONTAINER_KILLED ||
           state == CONTAINER_EXITED;
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

static int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1U) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

static int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return 0;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1U) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 1;
}

static container_record_t *find_container_locked(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *cur = ctx->containers;

    while (cur) {
        if (strncmp(cur->id, id, sizeof(cur->id)) == 0)
            return cur;
        cur = cur->next;
    }

    return NULL;
}

static container_record_t *find_container_by_pid_locked(supervisor_ctx_t *ctx, pid_t pid)
{
    container_record_t *cur = ctx->containers;

    while (cur) {
        if (cur->host_pid == pid)
            return cur;
        cur = cur->next;
    }

    return NULL;
}

static int running_uses_rootfs_locked(supervisor_ctx_t *ctx, const char *rootfs)
{
    container_record_t *cur = ctx->containers;

    while (cur) {
        if (!is_terminal_state(cur->state) &&
            strncmp(cur->rootfs, rootfs, sizeof(cur->rootfs)) == 0)
            return 1;
        cur = cur->next;
    }

    return 0;
}

static int append_text(char **buf,
                       size_t *capacity,
                       size_t *used,
                       const char *fmt,
                       ...)
{
    va_list ap;
    int needed;

    while (1) {
        if (*used >= *capacity) {
            size_t new_capacity = (*capacity == 0) ? 4096 : (*capacity * 2U);
            char *tmp = realloc(*buf, new_capacity);
            if (!tmp)
                return -1;
            *buf = tmp;
            *capacity = new_capacity;
        }

        va_start(ap, fmt);
        needed = vsnprintf(*buf + *used, *capacity - *used, fmt, ap);
        va_end(ap);

        if (needed < 0)
            return -1;

        if ((size_t)needed < (*capacity - *used)) {
            *used += (size_t)needed;
            return 0;
        }

        {
            size_t new_capacity = *capacity + (size_t)needed + 1U;
            char *tmp = realloc(*buf, new_capacity);
            if (!tmp)
                return -1;
            *buf = tmp;
            *capacity = new_capacity;
        }
    }
}

static int ensure_log_dir(void)
{
    struct stat st;

    if (stat(LOG_DIR, &st) == 0) {
        if (S_ISDIR(st.st_mode))
            return 0;
        errno = ENOTDIR;
        return -1;
    }

    return mkdir(LOG_DIR, 0755);
}

static int register_with_monitor(int monitor_fd,
                                 const char *container_id,
                                 pid_t host_pid,
                                 unsigned long soft_limit_bytes,
                                 unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    if (monitor_fd < 0) {
        errno = ENODEV;
        return -1;
    }

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

static int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    if (monitor_fd < 0)
        return 0;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

static int container_exit_status(const container_record_t *record)
{
    if (record->state == CONTAINER_KILLED)
        return 128 + SIGKILL;
    if (record->exit_signal > 0)
        return 128 + record->exit_signal;
    return record->exit_code;
}

static void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = arg;
    log_item_t item;

    while (bounded_buffer_pop(&ctx->log_buffer, &item) > 0) {
        char path[PATH_MAX];
        int fd;

        memset(path, 0, sizeof(path));

        pthread_mutex_lock(&ctx->metadata_lock);
        {
            container_record_t *record = find_container_locked(ctx, item.container_id);
            if (record)
                strncpy(path, record->log_path, sizeof(path) - 1);
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (path[0] == '\0')
            continue;

        fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd < 0)
            continue;

        if (write_all(fd, item.data, item.length) < 0) {
            close(fd);
            continue;
        }

        close(fd);
    }

    return NULL;
}

static void *producer_thread_main(void *arg)
{
    producer_arg_t *producer = arg;
    char chunk[LOG_CHUNK_SIZE];
    ssize_t nread;

    while ((nread = read(producer->read_fd, chunk, sizeof(chunk))) > 0) {
        log_item_t item;

        memset(&item, 0, sizeof(item));
        strncpy(item.container_id, producer->container_id, sizeof(item.container_id) - 1);
        item.length = (size_t)nread;
        memcpy(item.data, chunk, (size_t)nread);

        if (bounded_buffer_push(&producer->ctx->log_buffer, &item) != 0)
            break;
    }

    close(producer->read_fd);
    free(producer);
    return NULL;
}

static int child_fn(void *arg)
{
    child_config_t *cfg = arg;
    int devnull;

    if (sethostname(cfg->id, strlen(cfg->id)) < 0)
        perror("sethostname");

#ifdef __linux__
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
        perror("mount-private");
#endif

    if (chdir(cfg->rootfs) < 0) {
        perror("chdir rootfs");
        return 1;
    }

    if (chroot(".") < 0) {
        perror("chroot");
        return 1;
    }

    if (chdir("/") < 0) {
        perror("chdir /");
        return 1;
    }

    if (mkdir("/proc", 0555) < 0 && errno != EEXIST) {
        perror("mkdir /proc");
        return 1;
    }

#ifdef __linux__
    if (mount("proc", "/proc", "proc", 0, NULL) < 0)
        perror("mount /proc");
#endif

    if (setpriority(PRIO_PROCESS, 0, cfg->nice_value) < 0)
        perror("setpriority");

    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0 ||
        dup2(cfg->log_write_fd, STDERR_FILENO) < 0) {
        perror("dup2");
        return 1;
    }

    devnull = open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        close(devnull);
    }

    close(cfg->log_write_fd);

    execl("/bin/sh", "sh", "-c", cfg->command, (char *)NULL);
    perror("execl");
    return 127;
}

static pid_t launch_container_process(void *stack, child_config_t *cfg)
{
#ifdef __linux__
    int clone_flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    return clone(child_fn, (char *)stack + STACK_SIZE, clone_flags, cfg);
#else
    (void)stack;
    pid_t pid = fork();
    if (pid == 0)
        _exit(child_fn(cfg));
    return pid;
#endif
}

static int send_response(int fd,
                         int status,
                         int exit_status,
                         const char *message,
                         const char *payload)
{
    control_response_t response;
    size_t payload_length = payload ? strlen(payload) : 0U;

    memset(&response, 0, sizeof(response));
    response.status = status;
    response.exit_status = exit_status;
    response.payload_length = (uint32_t)payload_length;
    if (message)
        strncpy(response.message, message, sizeof(response.message) - 1);

    if (write_all(fd, &response, sizeof(response)) != (ssize_t)sizeof(response))
        return -1;

    if (payload_length > 0 &&
        write_all(fd, payload, payload_length) != (ssize_t)payload_length)
        return -1;

    return 0;
}

static int start_container(supervisor_ctx_t *ctx,
                           const control_request_t *req,
                           char *message,
                           size_t message_len,
                           container_record_t **record_out)
{
    int pipefd[2] = {-1, -1};
    child_config_t *cfg = NULL;
    void *stack = NULL;
    container_record_t *record = NULL;
    producer_arg_t *producer = NULL;
    pid_t child_pid;

    if (ensure_log_dir() < 0) {
        snprintf(message, message_len, "failed to create logs directory: %s",
                 strerror(errno));
        return -1;
    }

    if (access(req->rootfs, F_OK) != 0) {
        snprintf(message, message_len, "rootfs does not exist: %s", req->rootfs);
        return -1;
    }

    pthread_mutex_lock(&ctx->metadata_lock);
    if (find_container_locked(ctx, req->container_id) != NULL) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        snprintf(message, message_len, "container id already exists: %s", req->container_id);
        return -1;
    }
    if (running_uses_rootfs_locked(ctx, req->rootfs)) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        snprintf(message, message_len, "rootfs already used by a live container: %s",
                 req->rootfs);
        return -1;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (pipe(pipefd) < 0) {
        snprintf(message, message_len, "pipe failed: %s", strerror(errno));
        return -1;
    }

    cfg = calloc(1, sizeof(*cfg));
    record = calloc(1, sizeof(*record));
    producer = calloc(1, sizeof(*producer));
    stack = malloc(STACK_SIZE);
    if (!cfg || !record || !producer || !stack) {
        snprintf(message, message_len, "allocation failure");
        goto fail;
    }

    strncpy(cfg->id, req->container_id, sizeof(cfg->id) - 1);
    strncpy(cfg->rootfs, req->rootfs, sizeof(cfg->rootfs) - 1);
    strncpy(cfg->command, req->command, sizeof(cfg->command) - 1);
    cfg->nice_value = req->nice_value;
    cfg->log_write_fd = pipefd[1];

    child_pid = launch_container_process(stack, cfg);
    if (child_pid < 0) {
        snprintf(message, message_len, "clone failed: %s", strerror(errno));
        goto fail;
    }

    close(pipefd[1]);
    pipefd[1] = -1;

    strncpy(record->id, req->container_id, sizeof(record->id) - 1);
    strncpy(record->rootfs, req->rootfs, sizeof(record->rootfs) - 1);
    strncpy(record->command, req->command, sizeof(record->command) - 1);
    record->host_pid = child_pid;
    record->started_at = time(NULL);
    record->state = CONTAINER_RUNNING;
    record->soft_limit_bytes = req->soft_limit_bytes;
    record->hard_limit_bytes = req->hard_limit_bytes;
    record->log_read_fd = pipefd[0];
    record->child_stack = stack;
    snprintf(record->log_path, sizeof(record->log_path), LOG_DIR "/%s.log", record->id);

    producer->ctx = ctx;
    producer->read_fd = pipefd[0];
    strncpy(producer->container_id, record->id, sizeof(producer->container_id) - 1);

    pthread_mutex_lock(&ctx->metadata_lock);
    record->next = ctx->containers;
    ctx->containers = record;
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (pthread_create(&record->producer_thread, NULL, producer_thread_main, producer) != 0) {
        pthread_mutex_lock(&ctx->metadata_lock);
        if (ctx->containers == record) {
            ctx->containers = record->next;
        } else {
            container_record_t *cur = ctx->containers;
            while (cur && cur->next != record)
                cur = cur->next;
            if (cur)
                cur->next = record->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
        snprintf(message, message_len, "failed to start producer thread");
        goto fail;
    }
    record->producer_started = 1;

    if (register_with_monitor(ctx->monitor_fd,
                              record->id,
                              record->host_pid,
                              record->soft_limit_bytes,
                              record->hard_limit_bytes) == 0) {
        record->monitor_registered = 1;
    }

    free(cfg);
    snprintf(message, message_len, "container %s started with pid %d",
             record->id, record->host_pid);
    if (record_out)
        *record_out = record;
    return 0;

fail:
    if (pipefd[0] >= 0)
        close(pipefd[0]);
    if (pipefd[1] >= 0)
        close(pipefd[1]);
    free(cfg);
    free(producer);
    free(stack);
    free(record);
    return -1;
}

static void reap_children(supervisor_ctx_t *ctx)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        container_record_t *record = NULL;
        pthread_t producer_thread;
        void *child_stack = NULL;
        int join_needed = 0;
        int unregister_needed = 0;
        char id[CONTAINER_ID_LEN];

        memset(id, 0, sizeof(id));

        pthread_mutex_lock(&ctx->metadata_lock);
        record = find_container_by_pid_locked(ctx, pid);
        if (record) {
            strncpy(id, record->id, sizeof(id) - 1);

            if (WIFEXITED(status)) {
                record->exit_code = WEXITSTATUS(status);
                record->exit_signal = 0;
                record->state = record->stop_requested ? CONTAINER_STOPPED : CONTAINER_EXITED;
            } else if (WIFSIGNALED(status)) {
                record->exit_code = 0;
                record->exit_signal = WTERMSIG(status);
                if (record->stop_requested)
                    record->state = CONTAINER_STOPPED;
                else if (record->exit_signal == SIGKILL)
                    record->state = CONTAINER_KILLED;
                else
                    record->state = CONTAINER_EXITED;
            }

            if (record->monitor_registered) {
                unregister_needed = 1;
                record->monitor_registered = 0;
            }

            if (record->producer_started && !record->producer_joined) {
                join_needed = 1;
                producer_thread = record->producer_thread;
                record->producer_joined = 1;
            }

            child_stack = record->child_stack;
            record->child_stack = NULL;
            pthread_cond_broadcast(&ctx->metadata_cv);
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (record && unregister_needed)
            unregister_from_monitor(ctx->monitor_fd, id, pid);

        if (join_needed)
            pthread_join(producer_thread, NULL);

        free(child_stack);
    }
}

static void shutdown_running_containers(supervisor_ctx_t *ctx)
{
    container_record_t *cur;

    pthread_mutex_lock(&ctx->metadata_lock);
    cur = ctx->containers;
    while (cur) {
        if (!is_terminal_state(cur->state)) {
            cur->stop_requested = 1;
            kill(cur->host_pid, SIGTERM);
        }
        cur = cur->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
}

static char *build_ps_payload(supervisor_ctx_t *ctx)
{
    char *buf = NULL;
    size_t capacity = 0;
    size_t used = 0;
    container_record_t *cur;

    if (append_text(&buf, &capacity, &used,
                    "ID\tPID\tSTATE\tSOFT_MIB\tHARD_MIB\tEXIT\tSIGNAL\tSTARTED\tROOTFS\tCOMMAND\n") != 0)
        goto fail;

    pthread_mutex_lock(&ctx->metadata_lock);
    cur = ctx->containers;
    while (cur) {
        char timebuf[64];
        struct tm tm_value;

        localtime_r(&cur->started_at, &tm_value);
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm_value);

        if (append_text(&buf, &capacity, &used,
                        "%s\t%d\t%s\t%lu\t%lu\t%d\t%d\t%s\t%s\t%s\n",
                        cur->id,
                        cur->host_pid,
                        state_to_string(cur->state),
                        cur->soft_limit_bytes >> 20,
                        cur->hard_limit_bytes >> 20,
                        cur->exit_code,
                        cur->exit_signal,
                        timebuf,
                        cur->rootfs,
                        cur->command) != 0) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            goto fail;
        }
        cur = cur->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    return buf;

fail:
    free(buf);
    return NULL;
}

static char *build_log_payload(supervisor_ctx_t *ctx, const char *id, char *message, size_t message_len)
{
    container_record_t *record;
    char path[PATH_MAX];
    struct stat st;
    char *payload;
    int fd;

    memset(path, 0, sizeof(path));

    pthread_mutex_lock(&ctx->metadata_lock);
    record = find_container_locked(ctx, id);
    if (record)
        strncpy(path, record->log_path, sizeof(path) - 1);
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (path[0] == '\0') {
        snprintf(message, message_len, "unknown container id: %s", id);
        return NULL;
    }

    if (stat(path, &st) < 0) {
        snprintf(message, message_len, "no log file available yet for %s", id);
        return NULL;
    }

    payload = calloc(1, (size_t)st.st_size + 1U);
    if (!payload) {
        snprintf(message, message_len, "allocation failure");
        return NULL;
    }

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(message, message_len, "open failed: %s", strerror(errno));
        free(payload);
        return NULL;
    }

    if (read_all(fd, payload, (size_t)st.st_size) != st.st_size) {
        snprintf(message, message_len, "read failed: %s", strerror(errno));
        close(fd);
        free(payload);
        return NULL;
    }

    close(fd);
    snprintf(message, message_len, "logs for %s", id);
    return payload;
}

static int handle_stop(supervisor_ctx_t *ctx, const char *id, char *message, size_t message_len)
{
    container_record_t *record;

    pthread_mutex_lock(&ctx->metadata_lock);
    record = find_container_locked(ctx, id);
    if (!record) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        snprintf(message, message_len, "unknown container id: %s", id);
        return -1;
    }

    if (is_terminal_state(record->state)) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        snprintf(message, message_len, "container %s is already stopped", id);
        return 0;
    }

    record->stop_requested = 1;
    if (kill(record->host_pid, SIGTERM) < 0) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        snprintf(message, message_len, "failed to signal %s: %s", id, strerror(errno));
        return -1;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    snprintf(message, message_len, "stop requested for %s", id);
    return 0;
}

static void supervisor_signal_handler(int signo)
{
    if (signo == SIGCHLD)
        supervisor_got_sigchld = 1;
    if (signo == SIGINT || signo == SIGTERM)
        supervisor_should_stop = 1;
}

static void client_signal_handler(int signo)
{
    (void)signo;
    client_forward_stop = 1;
}

static int install_supervisor_signals(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = supervisor_signal_handler;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGCHLD, &sa, NULL) < 0)
        return -1;
    if (sigaction(SIGINT, &sa, NULL) < 0)
        return -1;
    if (sigaction(SIGTERM, &sa, NULL) < 0)
        return -1;

    signal(SIGPIPE, SIG_IGN);
    return 0;
}

static int install_client_run_signals(struct sigaction *old_int, struct sigaction *old_term)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = client_signal_handler;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, old_int) < 0)
        return -1;
    if (sigaction(SIGTERM, &sa, old_term) < 0) {
        sigaction(SIGINT, old_int, NULL);
        return -1;
    }

    return 0;
}

static void restore_client_signals(const struct sigaction *old_int,
                                   const struct sigaction *old_term)
{
    sigaction(SIGINT, old_int, NULL);
    sigaction(SIGTERM, old_term, NULL);
}

static void *request_thread_main(void *arg)
{
    request_thread_arg_t *thread_arg = arg;
    supervisor_ctx_t *ctx = thread_arg->ctx;
    int client_fd = thread_arg->client_fd;
    control_request_t req;
    char message[CONTROL_MESSAGE_LEN];

    memset(&req, 0, sizeof(req));
    memset(message, 0, sizeof(message));
    free(thread_arg);

    if (read_all(client_fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) {
        close(client_fd);
        return NULL;
    }

    if (req.kind == CMD_START || req.kind == CMD_RUN) {
        container_record_t *record = NULL;
        int rc = start_container(ctx, &req, message, sizeof(message), &record);
        if (rc != 0) {
            send_response(client_fd, 1, 1, message, NULL);
            close(client_fd);
            return NULL;
        }

        if (req.kind == CMD_START) {
            send_response(client_fd, 0, 0, message, NULL);
            close(client_fd);
            return NULL;
        }

        pthread_mutex_lock(&ctx->metadata_lock);
        while (!is_terminal_state(record->state))
            pthread_cond_wait(&ctx->metadata_cv, &ctx->metadata_lock);
        snprintf(message, sizeof(message),
                 "container %s finished with state=%s exit_status=%d",
                 record->id,
                 state_to_string(record->state),
                 container_exit_status(record));
        pthread_mutex_unlock(&ctx->metadata_lock);

        send_response(client_fd, 0, container_exit_status(record), message, NULL);
        close(client_fd);
        return NULL;
    }

    if (req.kind == CMD_PS) {
        char *payload = build_ps_payload(ctx);
        if (!payload) {
            send_response(client_fd, 1, 1, "failed to build ps output", NULL);
        } else {
            send_response(client_fd, 0, 0, "ok", payload);
            free(payload);
        }
        close(client_fd);
        return NULL;
    }

    if (req.kind == CMD_LOGS) {
        char *payload = build_log_payload(ctx, req.container_id, message, sizeof(message));
        if (!payload) {
            send_response(client_fd, 1, 1, message, NULL);
        } else {
            send_response(client_fd, 0, 0, message, payload);
            free(payload);
        }
        close(client_fd);
        return NULL;
    }

    if (req.kind == CMD_STOP) {
        int rc = handle_stop(ctx, req.container_id, message, sizeof(message));
        send_response(client_fd, rc == 0 ? 0 : 1, rc == 0 ? 0 : 1, message, NULL);
        close(client_fd);
        return NULL;
    }

    send_response(client_fd, 1, 1, "unsupported command", NULL);
    close(client_fd);
    return NULL;
}

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    struct sockaddr_un addr;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;
    strncpy(ctx.base_rootfs, rootfs, sizeof(ctx.base_rootfs) - 1);

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = pthread_cond_init(&ctx.metadata_cv, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_cond_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_cond_destroy(&ctx.metadata_cv);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0) {
        fprintf(stderr,
                "warning: could not open /dev/container_monitor (%s); memory enforcement disabled\n",
                strerror(errno));
    }

    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        goto fail;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        goto fail;
    }

    if (listen(ctx.server_fd, 32) < 0) {
        perror("listen");
        goto fail;
    }

    if (install_supervisor_signals() < 0) {
        perror("sigaction");
        goto fail;
    }

    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) {
        errno = rc;
        perror("pthread_create");
        goto fail;
    }

    printf("supervisor listening on %s (base-rootfs=%s)\n", CONTROL_PATH, ctx.base_rootfs);
    fflush(stdout);

    while (!ctx.should_stop) {
        struct pollfd pfd;
        int poll_rc;

        if (supervisor_should_stop)
            ctx.should_stop = 1;

        if (supervisor_got_sigchld) {
            supervisor_got_sigchld = 0;
            reap_children(&ctx);
        }

        if (ctx.should_stop)
            break;

        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = ctx.server_fd;
        pfd.events = POLLIN;
        poll_rc = poll(&pfd, 1, 500);
        if (poll_rc < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }

        if (poll_rc > 0 && (pfd.revents & POLLIN)) {
            int client_fd = accept(ctx.server_fd, NULL, NULL);
            if (client_fd >= 0) {
                pthread_t thread;
                request_thread_arg_t *arg = calloc(1, sizeof(*arg));
                if (!arg) {
                    close(client_fd);
                    continue;
                }
                arg->ctx = &ctx;
                arg->client_fd = client_fd;
                if (pthread_create(&thread, NULL, request_thread_main, arg) == 0) {
                    pthread_detach(thread);
                } else {
                    close(client_fd);
                    free(arg);
                }
            }
        }
    }

    shutdown_running_containers(&ctx);

    while (1) {
        int active = 0;
        container_record_t *cur;

        reap_children(&ctx);
        pthread_mutex_lock(&ctx.metadata_lock);
        cur = ctx.containers;
        while (cur) {
            if (!is_terminal_state(cur->state)) {
                active = 1;
                break;
            }
            cur = cur->next;
        }
        pthread_mutex_unlock(&ctx.metadata_lock);

        if (!active)
            break;
        usleep(100000);
    }

    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

    while (ctx.containers) {
        container_record_t *next = ctx.containers->next;
        if (ctx.containers->producer_started && !ctx.containers->producer_joined) {
            pthread_join(ctx.containers->producer_thread, NULL);
        }
        free(ctx.containers->child_stack);
        free(ctx.containers);
        ctx.containers = next;
    }

    if (ctx.monitor_fd >= 0)
        close(ctx.monitor_fd);
    if (ctx.server_fd >= 0)
        close(ctx.server_fd);
    unlink(CONTROL_PATH);

    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_cond_destroy(&ctx.metadata_cv);
    pthread_mutex_destroy(&ctx.metadata_lock);
    return 0;

fail:
    if (ctx.monitor_fd >= 0)
        close(ctx.monitor_fd);
    if (ctx.server_fd >= 0)
        close(ctx.server_fd);
    unlink(CONTROL_PATH);
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_cond_destroy(&ctx.metadata_cv);
    pthread_mutex_destroy(&ctx.metadata_lock);
    return 1;
}

static int request_stop_from_client(const char *container_id)
{
    control_request_t req;
    int fd;
    struct sockaddr_un addr;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    if (write_all(fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) {
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

static int read_response_with_optional_forwarding(int fd,
                                                  const control_request_t *req,
                                                  control_response_t *response,
                                                  char **payload_out)
{
    size_t total = 0;
    char *resp_bytes = (char *)response;
    char *payload = NULL;
    int stop_forwarded = 0;

    while (total < sizeof(*response)) {
        struct pollfd pfd;
        int rc;

        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = fd;
        pfd.events = POLLIN;

        rc = poll(&pfd, 1, 500);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (rc == 0) {
            if (req->kind == CMD_RUN && client_forward_stop && !stop_forwarded) {
                request_stop_from_client(client_run_id);
                stop_forwarded = 1;
            }
            continue;
        }

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            return -1;

        if (pfd.revents & POLLIN) {
            ssize_t nread = read(fd, resp_bytes + total, sizeof(*response) - total);
            if (nread < 0) {
                if (errno == EINTR)
                    continue;
                return -1;
            }
            if (nread == 0)
                return -1;
            total += (size_t)nread;
        }
    }

    if (response->payload_length > 0) {
        payload = calloc(1, response->payload_length + 1U);
        if (!payload)
            return -1;
        if (read_all(fd, payload, response->payload_length) != (ssize_t)response->payload_length) {
            free(payload);
            return -1;
        }
    }

    *payload_out = payload;
    return 0;
}

static int send_control_request(const control_request_t *req)
{
    int fd;
    struct sockaddr_un addr;
    control_response_t response;
    char *payload = NULL;
    int return_code = 1;
    struct sigaction old_int;
    struct sigaction old_term;
    int run_signal_installed = 0;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }

    if (req->kind == CMD_RUN) {
        client_forward_stop = 0;
        memset(client_run_id, 0, sizeof(client_run_id));
        strncpy(client_run_id, req->container_id, sizeof(client_run_id) - 1);
        if (install_client_run_signals(&old_int, &old_term) == 0)
            run_signal_installed = 1;
    }

    if (write_all(fd, req, sizeof(*req)) != (ssize_t)sizeof(*req)) {
        perror("write");
        goto out;
    }

    memset(&response, 0, sizeof(response));
    if (read_response_with_optional_forwarding(fd, req, &response, &payload) != 0) {
        perror("read");
        goto out;
    }

    if (payload && response.payload_length > 0)
        fwrite(payload, 1, response.payload_length, stdout);
    else if (response.message[0] != '\0')
        printf("%s\n", response.message);

    if (response.status != 0) {
        if (payload == NULL && response.message[0] != '\0')
            fprintf(stderr, "%s\n", response.message);
        return_code = 1;
    } else if (req->kind == CMD_RUN) {
        return_code = response.exit_status;
    } else {
        return_code = 0;
    }

out:
    if (run_signal_installed)
        restore_client_signals(&old_int, &old_term);
    free(payload);
    close(fd);
    return return_code;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
