// client.c - Mini Cloud Storage Client (C, POSIX, Ubuntu/WSL)
// Build: make
// Run:   ./client <server_ip> <port>
// Example: ./client 127.0.0.1 8080
//
// Commands at prompt:
//   list
//   upload <localpath> [remote_name]
//   download <remote_name> [save_as]
//   rename <oldname> <newname>
//   delete <remote_name>
//   quit

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_LINE 4096
#define BUF_SIZE (1<<16)

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
        if (n == 0) break;
        recvd += (size_t)n;
    }
    return (ssize_t)recvd;
}

static ssize_t recv_line(int fd, char *out, size_t cap) {
    size_t i = 0;
    while (i + 1 < cap) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) { if (i == 0) return 0; break; }
        out[i++] = c;
        if (c == '\n') break;
    }
    out[i] = '\0';
    return (ssize_t)i;
}

static void chomp(char *s) {
    size_t n = strlen(s);
    while (n && (s[n-1] == '\n' || s[n-1] == '\r')) s[--n] = 0;
}

static int send_line(int fd, const char *fmt, ...) {
    char buf[MAX_LINE];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    size_t len = strlen(buf);
    return (int)send_all(fd, buf, len);
}

static const char *basename2(const char *path) {
    const char *slash1 = strrchr(path, '/');
#ifdef _WIN32
    const char *slash2 = strrchr(path, '\\');
    if (!slash1 || (slash2 && slash2 > slash1)) slash1 = slash2;
#endif
    return slash1 ? slash1 + 1 : path;
}

static int do_list(int sfd) {
    if (send_line(sfd, "LIST\n") < 0) { perror("send"); return -1; }
    char line[MAX_LINE];
    if (recv_line(sfd, line, sizeof(line)) <= 0) { fprintf(stderr, "server closed\n"); return -1; }
    chomp(line);
    if (strncmp(line, "OK ", 3) != 0) { fprintf(stderr, "%s\n", line); return -1; }
    int count = 0;
    sscanf(line, "OK %d", &count);
    printf("Files (%d):\n", count);
    for (;;) {
        if (recv_line(sfd, line, sizeof(line)) <= 0) { fprintf(stderr, "server closed\n"); return -1; }
        chomp(line);
        if (strcmp(line, "END") == 0) break;
        if (strncmp(line, "FILE ", 5) == 0) {
            char name[1024]; long long sz = 0;
            if (sscanf(line, "FILE %1023s %lld", name, &sz) == 2) {
                printf("  %-30s %lld bytes\n", name, sz);
            }
        } else {
            printf("%s\n", line);
        }
    }
    return 0;
}

static int do_upload(int sfd, const char *local, const char *remote_opt) {
    const char *remote = remote_opt ? remote_opt : basename2(local);
    // get size
    struct stat st;
    if (stat(local, &st) < 0 || !S_ISREG(st.st_mode)) {
        fprintf(stderr, "Local file not found: %s\n", local);
        return -1;
    }
    long long size = (long long)st.st_size;
    int fd = open(local, O_RDONLY);
    if (fd < 0) { perror("open"); return -1; }

    if (send_line(sfd, "UPLOAD %s %lld\n", remote, size) < 0) { perror("send"); close(fd); return -1; }

    char line[MAX_LINE];
    if (recv_line(sfd, line, sizeof(line)) <= 0) { fprintf(stderr, "server closed\n"); close(fd); return -1; }
    chomp(line);
    if (strcmp(line, "OK") != 0) { fprintf(stderr, "%s\n", line); close(fd); return -1; }

    char *buf = malloc(BUF_SIZE);
    if (!buf) { fprintf(stderr, "oom\n"); close(fd); return -1; }
    ssize_t n;
    long long sent = 0;
    while ((n = read(fd, buf, BUF_SIZE)) > 0) {
        if (send_all(sfd, buf, (size_t)n) != n) {
            perror("send data");
            free(buf); close(fd); return -1;
        }
        sent += n;
    }
    free(buf);
    close(fd);

    if (sent != size) {
        fprintf(stderr, "Upload mismatch: sent %lld of %lld\n", sent, size);
        return -1;
    }
    if (recv_line(sfd, line, sizeof(line)) <= 0) { fprintf(stderr, "server closed\n"); return -1; }
    chomp(line);
    if (strncmp(line, "OK", 2) == 0) {
        printf("Upload complete: %s (%lld bytes)\n", remote, size);
        return 0;
    } else {
        fprintf(stderr, "%s\n", line);
        return -1;
    }
}

static int do_download(int sfd, const char *remote, const char *save_as_opt) {
    if (send_line(sfd, "DOWNLOAD %s\n", remote) < 0) { perror("send"); return -1; }
    char line[MAX_LINE];
    if (recv_line(sfd, line, sizeof(line)) <= 0) { fprintf(stderr, "server closed\n"); return -1; }
    chomp(line);
    if (strncmp(line, "OK ", 3) != 0) { fprintf(stderr, "%s\n", line); return -1; }
    long long size = 0;
    sscanf(line, "OK %lld", &size);

    const char *save_as = save_as_opt ? save_as_opt : remote;
    int fd = open(save_as, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open save_as"); return -1; }

    char *buf = malloc(BUF_SIZE);
    if (!buf) { fprintf(stderr, "oom\n"); close(fd); return -1; }

    long long remaining = size;
    while (remaining > 0) {
        size_t chunk = (remaining > BUF_SIZE) ? BUF_SIZE : (size_t)remaining;
        ssize_t n = recv_all(sfd, buf, chunk);
        if (n <= 0) {
            fprintf(stderr, "recv data failed\n");
            free(buf); close(fd); return -1;
        }
        ssize_t w = write(fd, buf, (size_t)n);
        if (w != n) {
            fprintf(stderr, "write failed\n");
            free(buf); close(fd); return -1;
        }
        remaining -= n;
    }
    free(buf);
    close(fd);

    printf("Downloaded %s (%lld bytes) -> %s\n", remote, size, save_as);
    return 0;
}

static int do_rename_remote(int sfd, const char *oldn, const char *newn) {
    if (send_line(sfd, "RENAME %s %s\n", oldn, newn) < 0) { perror("send"); return -1; }
    char line[MAX_LINE];
    if (recv_line(sfd, line, sizeof(line)) <= 0) { fprintf(stderr, "server closed\n"); return -1; }
    chomp(line);
    if (strncmp(line, "OK", 2) == 0) { printf("Renamed.\n"); return 0; }
    fprintf(stderr, "%s\n", line);
    return -1;
}

static int do_delete_remote(int sfd, const char *name) {
    if (send_line(sfd, "DELETE %s\n", name) < 0) { perror("send"); return -1; }
    char line[MAX_LINE];
    if (recv_line(sfd, line, sizeof(line)) <= 0) { fprintf(stderr, "server closed\n"); return -1; }
    chomp(line);
    if (strncmp(line, "OK", 2) == 0) { printf("Deleted.\n"); return 0; }
    fprintf(stderr, "%s\n", line);
    return -1;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <server_ip> <port>\n", argv[0]);
        return 1;
    }
    const char *ip = argv[1];
    int port = atoi(argv[2]);

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid IP\n");
        close(sfd);
        return 1;
    }

    if (connect(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sfd);
        return 1;
    }

    // Show server greeting
    char line[MAX_LINE];
    if (recv_line(sfd, line, sizeof(line)) > 0) {
        fputs(line, stdout);
    }

    // Simple REPL
    for (;;) {
        printf("cloud> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        char cmd[64], a1[1024], a2[1024];
        memset(cmd,0,sizeof(cmd)); memset(a1,0,sizeof(a1)); memset(a2,0,sizeof(a2));

        if (sscanf(line, "list") == 0 && strncmp(line, "list", 4) == 0) {
            if (do_list(sfd) < 0) {}
        }
        else if (sscanf(line, "upload %1023s %1023s", a1, a2) == 2) {
            do_upload(sfd, a1, a2);
        }
        else if (sscanf(line, "upload %1023s", a1) == 1) {
            do_upload(sfd, a1, NULL);
        }
        else if (sscanf(line, "download %1023s %1023s", a1, a2) == 2) {
            do_download(sfd, a1, a2);
        }
        else if (sscanf(line, "download %1023s", a1) == 1) {
            do_download(sfd, a1, NULL);
        }
        else if (sscanf(line, "rename %1023s %1023s", a1, a2) == 2) {
            do_rename_remote(sfd, a1, a2);
        }
        else if (sscanf(line, "delete %1023s", a1) == 1) {
            do_delete_remote(sfd, a1);
        }
        else if (strncmp(line, "quit", 4) == 0) {
            send_line(sfd, "QUIT\n");
            if (recv_line(sfd, line, sizeof(line)) > 0) fputs(line, stdout);
            break;
        }
        else if (strncmp(line, "\n", 1) == 0) {
            continue;
        }
        else {
            printf("Commands:\n");
            printf("  list\n");
            printf("  upload <localpath> [remote_name]\n");
            printf("  download <remote_name> [save_as]\n");
            printf("  rename <oldname> <newname>\n");
            printf("  delete <remote_name>\n");
            printf("  quit\n");
        }
    }

    close(sfd);
    return 0;
}

