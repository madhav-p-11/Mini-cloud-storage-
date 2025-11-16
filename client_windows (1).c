#define _WIN32_WINNT 0x0600
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

#define MAX_LINE 4096
#define BUF_SIZE (1<<16)

static int send_all(SOCKET sock, const char* buf, int len) {
    int total = 0;
    while (total < len) {
        int sent = send(sock, buf + total, len - total, 0);
        if (sent == SOCKET_ERROR) {
            return SOCKET_ERROR;
        }
        total += sent;
    }
    return total;
}

static int recv_all(SOCKET sock, char* buf, int len) {
    int total = 0;
    while (total < len) {
        int received = recv(sock, buf + total, len - total, 0);
        if (received == SOCKET_ERROR || received == 0) {
            return SOCKET_ERROR;
        }
        total += received;
    }
    return total;
}

static int recv_line(SOCKET sock, char* buf, int maxlen) {
    int i = 0;
    while (i < maxlen - 1) {
        char c;
        int received = recv(sock, &c, 1, 0);
        if (received <= 0) return -1;
        buf[i++] = c;
        if (c == '\n') break;
    }
    buf[i] = '\0';
    return i;
}

static void chomp(char* s) {
    int n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
}

static int send_line(SOCKET sock, const char* fmt, ...) {
    char buf[MAX_LINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return send_all(sock, buf, strlen(buf));
}

static const char* basename2(const char* path) {
    const char* slash = strrchr(path, '\\');
    return slash ? slash + 1 : path;
}

static int do_list(SOCKET sock) {
    if (send_line(sock, "LIST\n") < 0) {
        printf("Failed to send LIST command\n");
        return -1;
    }

    char line[MAX_LINE];
    if (recv_line(sock, line, sizeof(line)) <= 0) {
        printf("Server closed connection\n");
        return -1;
    }

    chomp(line);
    if (strncmp(line, "OK ", 3) != 0) {
        printf("%s\n", line);
        return -1;
    }

    int count = 0;
    sscanf(line, "OK %d", &count);
    printf("Files (%d):\n", count);

    while (1) {
        if (recv_line(sock, line, sizeof(line)) <= 0) {
            printf("Server closed connection\n");
            return -1;
        }
        chomp(line);
        if (strcmp(line, "END") == 0) break;

        if (strncmp(line, "FILE ", 5) == 0) {
            char name[1024];
            long long size = 0;
            if (sscanf(line, "FILE %1023s %lld", name, &size) == 2) {
                printf("  %-30s %lld bytes\n", name, size);
            }
        } else {
            printf("%s\n", line);
        }
    }
    return 0;
}

static int do_upload(SOCKET sock, const char* local, const char* remote_opt) {
    const char* remote = remote_opt ? remote_opt : basename2(local);

    // Get file size
    HANDLE hFile = CreateFile(local, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        printf("Cannot open local file: %s\n", local);
        return -1;
    }

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize)) {
        printf("Cannot get file size\n");
        CloseHandle(hFile);
        return -1;
    }

    if (send_line(sock, "UPLOAD %s %lld\n", remote, fileSize.QuadPart) < 0) {
        printf("Failed to send UPLOAD command\n");
        CloseHandle(hFile);
        return -1;
    }

    char line[MAX_LINE];
    if (recv_line(sock, line, sizeof(line)) <= 0) {
        printf("Server closed connection\n");
        CloseHandle(hFile);
        return -1;
    }
    chomp(line);
    if (strcmp(line, "OK") != 0) {
        printf("%s\n", line);
        CloseHandle(hFile);
        return -1;
    }

    char* buf = malloc(BUF_SIZE);
    if (!buf) {
        printf("Out of memory\n");
        CloseHandle(hFile);
        return -1;
    }

    long long sent = 0;
    while (1) {
        DWORD bytesRead;
        if (!ReadFile(hFile, buf, BUF_SIZE, &bytesRead, NULL) || bytesRead == 0) break;

        if (send_all(sock, buf, bytesRead) != bytesRead) {
            printf("Send error\n");
            free(buf);
            CloseHandle(hFile);
            return -1;
        }
        sent += bytesRead;
    }

    free(buf);
    CloseHandle(hFile);

    if (sent != fileSize.QuadPart) {
        printf("Upload mismatch: sent %lld of %lld\n", sent, fileSize.QuadPart);
        return -1;
    }

    if (recv_line(sock, line, sizeof(line)) <= 0) {
        printf("Server closed connection\n");
        return -1;
    }
    chomp(line);
    if (strncmp(line, "OK", 2) == 0) {
        printf("Upload complete: %s (%lld bytes)\n", remote, fileSize.QuadPart);
        return 0;
    } else {
        printf("%s\n", line);
        return -1;
    }
}

static int do_download(SOCKET sock, const char* remote, const char* save_as_opt) {
    if (send_line(sock, "DOWNLOAD %s\n", remote) < 0) {
        printf("Failed to send DOWNLOAD command\n");
        return -1;
    }

    char line[MAX_LINE];
    if (recv_line(sock, line, sizeof(line)) <= 0) {
        printf("Server closed connection\n");
        return -1;
    }
    chomp(line);
    if (strncmp(line, "OK ", 3) != 0) {
        printf("%s\n", line);
        return -1;
    }

    long long size = 0;
    sscanf(line, "OK %lld", &size);

    const char* save_as = save_as_opt ? save_as_opt : remote;
    HANDLE hFile = CreateFile(save_as, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        printf("Cannot create file: %s\n", save_as);
        return -1;
    }

    char* buf = malloc(BUF_SIZE);
    if (!buf) {
        printf("Out of memory\n");
        CloseHandle(hFile);
        return -1;
    }

    long long remaining = size;
    while (remaining > 0) {
        int chunk = (remaining > BUF_SIZE) ? BUF_SIZE : (int)remaining;
        int received = recv_all(sock, buf, chunk);
        if (received <= 0) {
            printf("Receive error\n");
            free(buf);
            CloseHandle(hFile);
            return -1;
        }

        DWORD written;
        if (!WriteFile(hFile, buf, received, &written, NULL) || written != received) {
            printf("Write error\n");
            free(buf);
            CloseHandle(hFile);
            return -1;
        }
        remaining -= received;
    }

    free(buf);
    CloseHandle(hFile);
    printf("Downloaded %s (%lld bytes) -> %s\n", remote, size, save_as);
    return 0;
}

static int do_rename_remote(SOCKET sock, const char* oldname, const char* newname) {
    if (send_line(sock, "RENAME %s %s\n", oldname, newname) < 0) {
        printf("Failed to send RENAME command\n");
        return -1;
    }

    char line[MAX_LINE];
    if (recv_line(sock, line, sizeof(line)) <= 0) {
        printf("Server closed connection\n");
        return -1;
    }
    chomp(line);
    if (strncmp(line, "OK", 2) == 0) {
        printf("Renamed.\n");
        return 0;
    }
    printf("%s\n", line);
    return -1;
}

static int do_delete_remote(SOCKET sock, const char* name) {
    if (send_line(sock, "DELETE %s\n", name) < 0) {
        printf("Failed to send DELETE command\n");
        return -1;
    }

    char line[MAX_LINE];
    if (recv_line(sock, line, sizeof(line)) <= 0) {
        printf("Server closed connection\n");
        return -1;
    }
    chomp(line);
    if (strncmp(line, "OK", 2) == 0) {
        printf("Deleted.\n");
        return 0;
    }
    printf("%s\n", line);
    return -1;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("Usage: %s <server_ip> <port>\n", argv[0]);
        return 1;
    }

    // Initialize Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("WSAStartup failed\n");
        return 1;
    }

    const char* ip = argv[1];
    int port = atoi(argv[2]);

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        printf("Failed to create socket\n");
        WSACleanup();
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = inet_addr(ip);
    if (addr.sin_addr.s_addr == INADDR_NONE) {
        printf("Invalid IP address\n");
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("Failed to connect to %s:%d\n", ip, port);
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    // Show server greeting
    char line[MAX_LINE];
    if (recv_line(sock, line, sizeof(line)) > 0) {
        printf("%s", line);
    }

    // Command loop
    while (1) {
        printf("cloud> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;

        char cmd[64], a1[1024], a2[1024];
        memset(cmd, 0, sizeof(cmd));
        memset(a1, 0, sizeof(a1));
        memset(a2, 0, sizeof(a2));

        if (sscanf(line, "list") == 0 && strncmp(line, "list", 4) == 0) {
            do_list(sock);
        }
        else if (sscanf(line, "upload %1023s %1023s", a1, a2) == 2) {
            do_upload(sock, a1, a2);
        }
        else if (sscanf(line, "upload %1023s", a1) == 1) {
            do_upload(sock, a1, NULL);
        }
        else if (sscanf(line, "download %1023s %1023s", a1, a2) == 2) {
            do_download(sock, a1, a2);
        }
        else if (sscanf(line, "download %1023s", a1) == 1) {
            do_download(sock, a1, NULL);
        }
        else if (sscanf(line, "rename %1023s %1023s", a1, a2) == 2) {
            do_rename_remote(sock, a1, a2);
        }
        else if (sscanf(line, "delete %1023s", a1) == 1) {
            do_delete_remote(sock, a1);
        }
        else if (strncmp(line, "quit", 4) == 0) {
            send_line(sock, "QUIT\n");
            if (recv_line(sock, line, sizeof(line)) > 0) printf("%s", line);
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

    closesocket(sock);
    WSACleanup();
    return 0;
}