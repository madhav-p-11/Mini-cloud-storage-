#define _WIN32_WINNT 0x0600
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>
#include <errno.h>

#pragma comment(lib, "ws2_32.lib")

#define BACKLOG 64
#define MAX_LINE 4096
#define STORAGE_PATH_MAX 1024

typedef struct {
    SOCKET client_socket;
    char storage_dir[STORAGE_PATH_MAX];
} client_ctx_t;

static volatile int running = 1;

static void die(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

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
        if (received == SOCKET_ERROR || received == 0) {
            return SOCKET_ERROR;
        }
        buf[i++] = c;
        if (c == '\n') break;
    }
    buf[i] = '\0';
    return i;
}

static int send_line(SOCKET sock, const char* fmt, ...) {
    char buf[MAX_LINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return send_all(sock, buf, strlen(buf));
}

static void chomp(char* s) {
    int n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[--n] = 0;
    }
}

static BOOL path_join(char* out, size_t cap, const char* dir, const char* name) {
    if (strstr(name, "..") != NULL || strchr(name, '/') != NULL || strchr(name, '\\') != NULL) {
        return FALSE;
    }
    int r = _snprintf(out, cap, "%s\\%s", dir, name);
    return (r > 0 && (size_t)r < cap);
}

static int handle_list(SOCKET sock, const char* storage_dir) {
    WIN32_FIND_DATA find_data;
    char search_path[MAX_PATH];
    snprintf(search_path, sizeof(search_path), "%s\\*", storage_dir);
    
    HANDLE hFind = FindFirstFile(search_path, &find_data);
    if (hFind == INVALID_HANDLE_VALUE) {
        send_line(sock, "ERR cannot open storage\n");
        return -1;
    }

    // Count files first
    int count = 0;
    do {
        if (strcmp(find_data.cFileName, ".") && strcmp(find_data.cFileName, "..")) {
            if (!(find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                count++;
            }
        }
    } while (FindNextFile(hFind, &find_data));
    
    FindClose(hFind);
    send_line(sock, "OK %d\n", count);

    // List files
    hFind = FindFirstFile(search_path, &find_data);
    do {
        if (strcmp(find_data.cFileName, ".") && strcmp(find_data.cFileName, "..")) {
            if (!(find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                LARGE_INTEGER filesize;
                filesize.LowPart = find_data.nFileSizeLow;
                filesize.HighPart = find_data.nFileSizeHigh;
                send_line(sock, "FILE %s %lld\n", find_data.cFileName, filesize.QuadPart);
            }
        }
    } while (FindNextFile(hFind, &find_data));

    FindClose(hFind);
    send_line(sock, "END\n");
    return 0;
}

static int handle_upload(SOCKET sock, const char* storage_dir, char* filename, long long size) {
    if (size < 0) {
        send_line(sock, "ERR invalid size\n");
        return -1;
    }

    char path[MAX_PATH];
    if (!path_join(path, sizeof(path), storage_dir, filename)) {
        send_line(sock, "ERR bad filename\n");
        return -1;
    }

    HANDLE hFile = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        send_line(sock, "ERR cannot open file for write\n");
        return -1;
    }

    send_line(sock, "OK\n");

    const int BUF_SIZE = 65536;
    char* buf = (char*)malloc(BUF_SIZE);
    if (!buf) {
        CloseHandle(hFile);
        send_line(sock, "ERR server oom\n");
        return -1;
    }

    long long remaining = size;
    while (remaining > 0) {
        int chunk = (remaining > BUF_SIZE) ? BUF_SIZE : (int)remaining;
        int received = recv_all(sock, buf, chunk);
        if (received == SOCKET_ERROR) {
            free(buf);
            CloseHandle(hFile);
            send_line(sock, "ERR recv data failed\n");
            return -1;
        }

        DWORD written;
        if (!WriteFile(hFile, buf, received, &written, NULL) || written != received) {
            free(buf);
            CloseHandle(hFile);
            send_line(sock, "ERR write failed\n");
            return -1;
        }

        remaining -= received;
    }

    free(buf);
    CloseHandle(hFile);
    send_line(sock, "OK SAVED\n");
    return 0;
}

static int handle_download(SOCKET sock, const char* storage_dir, char* filename) {
    char path[MAX_PATH];
    if (!path_join(path, sizeof(path), storage_dir, filename)) {
        send_line(sock, "ERR bad filename\n");
        return -1;
    }

    HANDLE hFile = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        send_line(sock, "ERR not found\n");
        return -1;
    }

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize)) {
        CloseHandle(hFile);
        send_line(sock, "ERR stat failed\n");
        return -1;
    }

    send_line(sock, "OK %lld\n", fileSize.QuadPart);

    const int BUF_SIZE = 65536;
    char* buf = (char*)malloc(BUF_SIZE);
    if (!buf) {
        CloseHandle(hFile);
        return -1;
    }

    while (1) {
        DWORD read;
        if (!ReadFile(hFile, buf, BUF_SIZE, &read, NULL)) {
            free(buf);
            CloseHandle(hFile);
            return -1;
        }
        if (read == 0) break;

        if (send_all(sock, buf, read) == SOCKET_ERROR) {
            free(buf);
            CloseHandle(hFile);
            return -1;
        }
    }

    free(buf);
    CloseHandle(hFile);
    return 0;
}

static int handle_rename(SOCKET sock, const char* storage_dir, char* oldname, char* newname) {
    char oldpath[MAX_PATH], newpath[MAX_PATH];
    if (!path_join(oldpath, sizeof(oldpath), storage_dir, oldname) ||
        !path_join(newpath, sizeof(newpath), storage_dir, newname)) {
        send_line(sock, "ERR bad filename\n");
        return -1;
    }

    if (!MoveFile(oldpath, newpath)) {
        send_line(sock, "ERR rename failed\n");
        return -1;
    }

    send_line(sock, "OK RENAMED\n");
    return 0;
}

static int handle_delete(SOCKET sock, const char* storage_dir, char* filename) {
    char path[MAX_PATH];
    if (!path_join(path, sizeof(path), storage_dir, filename)) {
        send_line(sock, "ERR bad filename\n");
        return -1;
    }

    if (!DeleteFile(path)) {
        send_line(sock, "ERR delete failed\n");
        return -1;
    }

    send_line(sock, "OK DELETED\n");
    return 0;
}

static DWORD WINAPI client_thread(LPVOID arg) {
    client_ctx_t* ctx = (client_ctx_t*)arg;
    SOCKET sock = ctx->client_socket;
    char line[MAX_LINE];

    send_line(sock, "OK WELCOME\n");

    while (1) {
        int n = recv_line(sock, line, sizeof(line));
        if (n == SOCKET_ERROR) break;
        chomp(line);
        if (line[0] == '\0') continue;

        char cmd[32], a1[MAX_PATH], a2[MAX_PATH];
        long long size = -1;
        memset(cmd, 0, sizeof(cmd));
        memset(a1, 0, sizeof(a1));
        memset(a2, 0, sizeof(a2));

        if (sscanf(line, "LIST") == 0 && strncmp(line, "LIST", 4) == 0) {
            handle_list(sock, ctx->storage_dir);
        }
        else if (sscanf(line, "UPLOAD %1023s %lld", a1, &size) == 2) {
            handle_upload(sock, ctx->storage_dir, a1, size);
        }
        else if (sscanf(line, "DOWNLOAD %1023s", a1) == 1) {
            handle_download(sock, ctx->storage_dir, a1);
        }
        else if (sscanf(line, "RENAME %1023s %1023s", a1, a2) == 2) {
            handle_rename(sock, ctx->storage_dir, a1, a2);
        }
        else if (sscanf(line, "DELETE %1023s", a1) == 1) {
            handle_delete(sock, ctx->storage_dir, a1);
        }
        else if (strncmp(line, "QUIT", 4) == 0) {
            send_line(sock, "OK BYE\n");
            break;
        }
        else {
            send_line(sock, "ERR unknown command\n");
        }
    }

    closesocket(sock);
    free(ctx);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <port> [storage_dir]\n", argv[0]);
        return 1;
    }

    // Initialize Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        die("WSAStartup failed");
    }

    int port = atoi(argv[1]);
    const char* storage_dir = (argc >= 3) ? argv[2] : "storage";

    // Ensure storage dir exists
    if (_mkdir(storage_dir) < 0 && errno != EEXIST) {
        die("Failed to create storage dir: %s", storage_dir);
    }

    SOCKET server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_socket == INVALID_SOCKET) {
        die("socket failed");
    }

    BOOL yes = TRUE;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server_socket, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        die("bind failed");
    }

    if (listen(server_socket, BACKLOG) == SOCKET_ERROR) {
        die("listen failed");
    }

    printf("Server listening on port %d, storage: %s\n", port, storage_dir);

    while (running) {
        struct sockaddr_in client_addr;
        int client_addr_len = sizeof(client_addr);
        SOCKET client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_addr_len);
        
        if (client_socket == INVALID_SOCKET) {
            if (WSAGetLastError() == WSAEINTR) continue;
            perror("accept");
            break;
        }

        client_ctx_t* ctx = (client_ctx_t*)malloc(sizeof(client_ctx_t));
        ctx->client_socket = client_socket;
        strncpy(ctx->storage_dir, storage_dir, sizeof(ctx->storage_dir) - 1);
        ctx->storage_dir[sizeof(ctx->storage_dir) - 1] = '\0';

        HANDLE thread = CreateThread(NULL, 0, client_thread, ctx, 0, NULL);
        if (thread == NULL) {
            perror("CreateThread");
            closesocket(client_socket);
            free(ctx);
            continue;
        }
        CloseHandle(thread);
    }

    closesocket(server_socket);
    WSACleanup();
    printf("Server shutting down.\n");
    return 0;
}