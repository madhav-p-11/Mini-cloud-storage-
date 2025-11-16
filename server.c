// server.c - Mini Cloud Storage Server (C, POSIX, Ubuntu/WSL)
// Build: make
// Run:   ./server <port> [storage_dir]
// Example: ./server 8080 storage
//
// Protocol (client -> server):
//   LIST
//   UPLOAD <filename> <size>
//   DOWNLOAD <filename>
//   RENAME <oldname> <newname>
//   DELETE <filename>
//   QUIT
//
// Responses:
//   On success: "OK ..." lines followed by data when applicable
//   On error:   "ERR <message>\n"
//
// Concurrency: Each client handled by a thread. File ops use fcntl() advisory locks.

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>

#define BACKLOG 64
#define MAX_LINE 4096
#define MAX_PATH 1024

typedef struct {
    int client_fd;
    char storage_dir[MAX_PATH];
} client_ctx_t;

static volatile sig_atomic_t running = 1;

static void on_sigint(int sig) {
    (void)sig;
    running = 0;
}

static void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

static ssize_t send_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break;
        sent += (size_t)n;
    }
    return (ssize_t)sent;
}

static ssize_t recv_all(int fd, void *buf, size_t len) {
    char *p = (char *)buf;
    size_t recvd = 0;
    while (recvd < len) {
        ssize_t n = recv(fd, p + recvd, len - recvd, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break; // connection closed
        recvd += (size_t)n;
    }
    return (ssize_t)recvd;
}

// Read a line ending with '\n' (up to MAX_LINE-1). Returns bytes read, 0 on EOF, -1 on error.
static ssize_t recv_line(int fd, char *out, size_t cap) {
    size_t i = 0;
    while (i + 1 < cap) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) { // peer closed
            if (i == 0) return 0;
            break;
        }
        out[i++] = c;
        if (c == '\n') break;
    }
    out[i] = '\0';
    return (ssize_t)i;
}

static int send_line(int fd, const char *fmt, ...) {
    char buf[MAX_LINE];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    size_t len = strlen(buf);
    return (int)send_all(fd, buf, len);
}

static void chomp(char *s) {
    size_t n = strlen(s);
    while (n && (s[n-1] == '\n' || s[n-1] == '\r')) {
        s[--n] = 0;
    }
}

static bool path_join(char *out, size_t cap, const char *dir, const char *name) {
    // Reject traversal
    if (strstr(name, "..") != NULL || strchr(name, '/') != NULL || strchr(name, '\\') != NULL) {
        return false;
    }
    int r = snprintf(out, cap, "%s/%s", dir, name);
    return (r > 0 && (size_t)r < cap);
}

static int lock_fd(int fd, short type) {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = type;   // F_RDLCK or F_WRLCK
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;       // lock whole file
    return fcntl(fd, F_SETLKW, &fl);
}

static int unlock_fd(int fd) {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    return fcntl(fd, F_SETLK, &fl);
}

static int handle_list(int cfd, const char *storage_dir) {
    DIR *d = opendir(storage_dir);
    if (!d) {
        send_line(cfd, "ERR cannot open storage\n");
        return -1;
    }
    // Count items
    int count = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) count++;
    }
    rewinddir(d);
    send_line(cfd, "OK %d\n", count);

    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char path[MAX_PATH];
        if (!path_join(path, sizeof(path), storage_dir, de->d_name)) continue;
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            send_line(cfd, "FILE %s %lld\n", de->d_name, (long long)st.st_size);
        }
    }
    send_line(cfd, "END\n");
    closedir(d);
    return 0;
}

static int handle_upload(int cfd, const char *storage_dir, char *filename, long long size) {
    if (size < 0) {
        send_line(cfd, "ERR invalid size\n");
        return -1;
    }
    char path[MAX_PATH];
    if (!path_join(path, sizeof(path), storage_dir, filename)) {
        send_line(cfd, "ERR bad filename\n");
        return -1;
    }
    // Open with O_CREAT|O_TRUNC
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        send_line(cfd, "ERR cannot open file for write\n");
        return -1;
    }
    // Exclusive write lock during upload
    if (lock_fd(fd, F_WRLCK) < 0) {
        close(fd);
        send_line(cfd, "ERR cannot lock file\n");
        return -1;
    }

    send_line(cfd, "OK\n"); // tell client to start sending bytes

    const size_t BUF = 1 << 16;
    char *buf = (char *)malloc(BUF);
    if (!buf) {
        unlock_fd(fd); close(fd);
        send_line(cfd, "ERR server oom\n");
        return -1;
    }
    long long remaining = size;
    while (remaining > 0) {
        size_t chunk = (remaining > (long long)BUF) ? BUF : (size_t)remaining;
        ssize_t n = recv_all(cfd, buf, chunk);
        if (n <= 0) {
            free(buf);
            unlock_fd(fd); close(fd);
            send_line(cfd, "ERR recv data failed\n");
            return -1;
        }
        ssize_t w = write(fd, buf, (size_t)n);
        if (w != n) {
            free(buf);
            unlock_fd(fd); close(fd);
            send_line(cfd, "ERR write failed\n");
            return -1;
        }
        remaining -= n;
    }
    free(buf);
    fsync(fd);
    unlock_fd(fd);
    close(fd);
    send_line(cfd, "OK SAVED\n");
    return 0;
}

static int handle_download(int cfd, const char *storage_dir, char *filename) {
    char path[MAX_PATH];
    if (!path_join(path, sizeof(path), storage_dir, filename)) {
        send_line(cfd, "ERR bad filename\n");
        return -1;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        send_line(cfd, "ERR not found\n");
        return -1;
    }
    // Shared read lock
    if (lock_fd(fd, F_RDLCK) < 0) {
        close(fd);
        send_line(cfd, "ERR cannot lock file\n");
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0) {
        unlock_fd(fd); close(fd);
        send_line(cfd, "ERR stat failed\n");
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        unlock_fd(fd); close(fd);
        send_line(cfd, "ERR not a file\n");
        return -1;
    }
    long long size = (long long)st.st_size;
    send_line(cfd, "OK %lld\n", size);

    off_t offset = 0;
#ifdef __linux__
    // sendfile from file->socket is efficient on Linux
    while (offset < st.st_size) {
        ssize_t n = sendfile(cfd, fd, &offset, (size_t)(st.st_size - offset));
        if (n < 0) {
            if (errno == EINTR) continue;
            unlock_fd(fd); close(fd);
            return -1;
        }
        if (n == 0) break;
    }
#else
    // Fallback copy
    const size_t BUF = 1 << 16;
    char *buf = malloc(BUF);
    if (!buf) { unlock_fd(fd); close(fd); return -1; }
    ssize_t n;
    while ((n = read(fd, buf, BUF)) > 0) {
        if (send_all(cfd, buf, (size_t)n) != n) { free(buf); unlock_fd(fd); close(fd); return -1; }
    }
    free(buf);
#endif
    unlock_fd(fd);
    close(fd);
    return 0;
}

static int handle_rename(int cfd, const char *storage_dir, char *oldn, char *newn) {
    char oldp[MAX_PATH], newp[MAX_PATH];
    if (!path_join(oldp, sizeof(oldp), storage_dir, oldn) ||
        !path_join(newp, sizeof(newp), storage_dir, newn)) {
        send_line(cfd, "ERR bad filename\n");
        return -1;
    }
    // Lock the old file for write to prevent clashes
    int fd = open(oldp, O_RDWR);
    if (fd < 0) {
        send_line(cfd, "ERR not found\n");
        return -1;
    }
    if (lock_fd(fd, F_WRLCK) < 0) {
        close(fd);
        send_line(cfd, "ERR cannot lock\n");
        return -1;
    }
    int r = rename(oldp, newp);
    unlock_fd(fd);
    close(fd);
    if (r < 0) {
        send_line(cfd, "ERR rename failed\n");
        return -1;
    }
    send_line(cfd, "OK RENAMED\n");
    return 0;
}

static int handle_delete(int cfd, const char *storage_dir, char *filename) {
    char path[MAX_PATH];
    if (!path_join(path, sizeof(path), storage_dir, filename)) {
        send_line(cfd, "ERR bad filename\n");
        return -1;
    }
    // Lock file for write before delete (best effort)
    int fd = open(path, O_RDWR);
    if (fd >= 0) {
        if (lock_fd(fd, F_WRLCK) == 0) {
            // proceed
        }
    }
    int r = unlink(path);
    if (fd >= 0) {
        if (fd >= 0) unlock_fd(fd);
        close(fd);
    }
    if (r < 0) {
        send_line(cfd, "ERR delete failed\n");
        return -1;
    }
    send_line(cfd, "OK DELETED\n");
    return 0;
}

static void *client_thread(void *arg) {
    client_ctx_t ctx = *(client_ctx_t *)arg;
    free(arg);

    int cfd = ctx.client_fd;
    char line[MAX_LINE];

    send_line(cfd, "OK WELCOME\n");

    for (;;) {
        ssize_t n = recv_line(cfd, line, sizeof(line));
        if (n <= 0) break;
        chomp(line);
        if (line[0] == '\0') continue;

        // Parse
        char cmd[32], a1[MAX_PATH], a2[MAX_PATH];
        long long size = -1;
        memset(cmd, 0, sizeof(cmd));
        memset(a1, 0, sizeof(a1));
        memset(a2, 0, sizeof(a2));

        if (sscanf(line, "LIST") == 0 && strncmp(line, "LIST", 4) == 0) {
            handle_list(cfd, ctx.storage_dir);
        }
        else if (sscanf(line, "UPLOAD %1023s %lld", a1, &size) == 2) {
            handle_upload(cfd, ctx.storage_dir, a1, size);
        }
        else if (sscanf(line, "DOWNLOAD %1023s", a1) == 1) {
            handle_download(cfd, ctx.storage_dir, a1);
        }
        else if (sscanf(line, "RENAME %1023s %1023s", a1, a2) == 2) {
            handle_rename(cfd, ctx.storage_dir, a1, a2);
        }
        else if (sscanf(line, "DELETE %1023s", a1) == 1) {
            handle_delete(cfd, ctx.storage_dir, a1);
        }
        else if (strncmp(line, "QUIT", 4) == 0) {
            send_line(cfd, "OK BYE\n");
            break;
        }
        else {
            send_line(cfd, "ERR unknown command\n");
        }
    }

    close(cfd);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <port> [storage_dir]\n", argv[0]);
        return 1;
    }
    int port = atoi(argv[1]);
    const char *storage_dir = (argc >= 3) ? argv[2] : "storage";

    // Ensure storage dir exists
    if (mkdir(storage_dir, 0755) < 0 && errno != EEXIST) {
        die("Failed to create storage dir: %s", storage_dir);
    }

    signal(SIGINT, on_sigint);

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) die("socket failed");

    int yes = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) die("bind failed");
    if (listen(sfd, BACKLOG) < 0) die("listen failed");

    printf("Server listening on port %d, storage: %s\n", port, storage_dir);

    while (running) {
        struct sockaddr_in cli;
        socklen_t clilen = sizeof(cli);
        int cfd = accept(sfd, (struct sockaddr *)&cli, &clilen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        client_ctx_t *ctx = (client_ctx_t *)malloc(sizeof(client_ctx_t));
        ctx->client_fd = cfd;
        strncpy(ctx->storage_dir, storage_dir, sizeof(ctx->storage_dir)-1);
        ctx->storage_dir[sizeof(ctx->storage_dir)-1] = '\0';

        pthread_t th;
        if (pthread_create(&th, NULL, client_thread, ctx) != 0) {
            perror("pthread_create");
            close(cfd);
            free(ctx);
            continue;
        }
        pthread_detach(th);
    }

    close(sfd);
    printf("Server shutting down.\n");
    return 0;
}

