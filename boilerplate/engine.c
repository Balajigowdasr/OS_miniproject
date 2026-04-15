#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"

typedef struct container {
    char id[32];
    pid_t pid;
    char log_path[256];
    struct container *next;
} container_t;

container_t *containers = NULL;

void sigchld_handler(int sig);

/* ================= CHILD ================= */
int child_fn(void *arg)
{
    char **cfg = (char **)arg;

    if (chroot(cfg[0]) < 0) {
        perror("chroot");
        return 1;
    }

    if (chdir("/") < 0) {
        perror("chdir");
        return 1;
    }

    mkdir("/proc", 0555);

    if (mount("proc", "/proc", "proc", 0, NULL) < 0) {
        perror("mount");
    }

    execl(cfg[1], cfg[1], NULL);

    perror("exec failed");
    return 1;
}

/* ================= START ================= */
void start_container(const char *id, const char *rootfs, const char *cmd)
{
    char *args[2];
    args[0] = (char *)rootfs;
    args[1] = (char *)cmd;

    void *stack = malloc(STACK_SIZE);
    if (stack == NULL) {
        perror("malloc");
        return;
    }

    pid_t pid = clone(
        child_fn,
        (char *)stack + STACK_SIZE,
        CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
        args);

    if (pid < 0) {
        perror("clone");
        free(stack);
        return;
    }

    /* REGISTER WITH KERNEL MODULE */
    int fd = open("/dev/container_monitor", O_RDWR);
    if (fd >= 0) {
        struct monitor_request req;

        memset(&req, 0, sizeof(req));
        req.pid = pid;
        req.soft_limit_bytes = 48 * 1024 * 1024;
        req.hard_limit_bytes = 80 * 1024 * 1024;

        strncpy(req.container_id, id, sizeof(req.container_id) - 1);

        ioctl(fd, MONITOR_REGISTER, &req);
        close(fd);
    }

    container_t *c = malloc(sizeof(container_t));
    if (c == NULL) {
        perror("malloc");
        return;
    }

    memset(c, 0, sizeof(container_t));

    strncpy(c->id, id, sizeof(c->id) - 1);
    c->pid = pid;

    snprintf(c->log_path, sizeof(c->log_path), "logs/%s.log", id);

    c->next = containers;
    containers = c;

    FILE *f = fopen(c->log_path, "a");
    if (f) {
        fprintf(f, "Container %s started PID=%d\n", id, pid);
        fclose(f);
    }

    printf("Container %s started PID=%d\n", id, pid);
}

/* ================= STOP ================= */
void stop_container(const char *id, int client_fd)
{
    container_t *c = containers;
    container_t *prev = NULL;

    while (c) {
        if (strcmp(c->id, id) == 0) {

            kill(c->pid, SIGKILL);

            if (prev)
                prev->next = c->next;
            else
                containers = c->next;

            free(c);

            write(client_fd, "Stopped", 7);
            return;
        }

        prev = c;
        c = c->next;
    }

    write(client_fd, "Not found", 9);
}

/* ================= LIST ================= */
void list_containers(int client_fd)
{
    container_t *c = containers;
    char output[1024] = "";

    if (!c) {
        strcpy(output, "No containers\n");
    }

    while (c) {
        char line[128];

        snprintf(line, sizeof(line), "%s PID=%d\n", c->id, c->pid);

        if (strlen(output) + strlen(line) < sizeof(output)) {
            strcat(output, line);
        }

        c = c->next;
    }

    write(client_fd, output, strlen(output));
}

/* ================= LOGS ================= */
void show_logs(const char *id)
{
    char path[256];

    snprintf(path, sizeof(path), "logs/%s.log", id);

    FILE *f = fopen(path, "r");
    if (!f) {
        printf("No logs\n");
        return;
    }

    char line[256];

    while (fgets(line, sizeof(line), f)) {
        printf("%s", line);
    }

    fclose(f);
}

/* ================= SUPERVISOR ================= */
void run_supervisor()
{
    signal(SIGCHLD, sigchld_handler);

    mkdir(LOG_DIR, 0755);

    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));

    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    unlink(CONTROL_PATH);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return;
    }

    chmod(CONTROL_PATH, 0666);

    if (listen(server_fd, 5) < 0) {
        perror("listen");
        close(server_fd);
        return;
    }

    printf("Supervisor running...\n");

    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);

        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        char buffer[256] = {0};

        ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);
        if (n <= 0) {
            close(client_fd);
            continue;
        }

        char cmd[20] = {0};
        char id[32] = {0};
        char rootfs[128] = {0};
        char prog[128] = {0};

        sscanf(buffer, "%19s %31s %127s %127s", cmd, id, rootfs, prog);

        if (strcmp(cmd, "start") == 0) {
            start_container(id, rootfs, prog);
            write(client_fd, "OK", 2);
        }
        else if (strcmp(cmd, "ps") == 0) {
            list_containers(client_fd);
        }
        else if (strcmp(cmd, "stop") == 0) {
            stop_container(id, client_fd);
        }
        else {
            write(client_fd, "Unknown", 7);
        }

        close(client_fd);
    }
}

/* ================= CLIENT ================= */
void send_request(char *msg)
{
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);

    if (sock < 0) {
        perror("socket");
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));

    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return;
    }

    write(sock, msg, strlen(msg));

    char res[1024] = {0};
    read(sock, res, sizeof(res) - 1);

    printf("%s\n", res);

    close(sock);
}

/* ================= SIGNAL ================= */
void sigchld_handler(int sig)
{
    (void)sig;

    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

/* ================= MAIN ================= */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("Usage:\n");
        printf("  ./engine supervisor\n");
        printf("  ./engine start <id> <rootfs> <program>\n");
        printf("  ./engine ps\n");
        printf("  ./engine stop <id>\n");
        printf("  ./engine logs <id>\n");
        return 0;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        run_supervisor();
    }
    else if (strcmp(argv[1], "start") == 0) {

        if (argc < 5) {
            printf("Usage: ./engine start <id> <rootfs> <program>\n");
            return 1;
        }

        char msg[256];

        snprintf(msg, sizeof(msg),
                 "start %s %s %s",
                 argv[2], argv[3], argv[4]);

        send_request(msg);
    }
    else if (strcmp(argv[1], "ps") == 0) {
        send_request("ps");
    }
    else if (strcmp(argv[1], "stop") == 0) {

        if (argc < 3) {
            printf("Usage: ./engine stop <id>\n");
            return 1;
        }

        char msg[256];

        snprintf(msg, sizeof(msg), "stop %s", argv[2]);

        send_request(msg);
    }
    else if (strcmp(argv[1], "logs") == 0) {

        if (argc < 3) {
            printf("Usage: ./engine logs <id>\n");
            return 1;
        }

        show_logs(argv[2]);
    }
    else {
        printf("Unknown command: %s\n", argv[1]);
        return 1;
    }

    return 0;
}
