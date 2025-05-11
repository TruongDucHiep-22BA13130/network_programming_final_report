
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <fcntl.h>

#define MAX_PEERS 10
#define MAX_FILES 100
#define BUFFER_SIZE 1024

const char *shared_dir = "shared";
const char *save_dir = "save";
char *peer_ips[MAX_PEERS];
int peer_ports[MAX_PEERS];
int peer_count = 0;
int listen_port;

void* listener_thread(void *arg);
void handle_client(int client_sock);
void list_local_files();
int send_file(const char *filename, int sock);
int receive_file(const char *filename, int sock);

void trim_newline(char *str) {
    str[strcspn(str, "\n")] = 0;
}

int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}


void* listener_thread(void *arg) {
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(listen_port);

    bind(server_sock, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_sock, 5);

    while (1) {
        int client_sock = accept(server_sock, NULL, NULL);
        if (client_sock >= 0) handle_client(client_sock);
    }
    return NULL;
}

void handle_client(int client_sock) {
    char buf[BUFFER_SIZE];
    int n = recv(client_sock, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return;
    buf[n] = '\0';

    if (strncmp(buf, "LIST", 4) == 0) {
        DIR *d = opendir(shared_dir);
        struct dirent *dir;
        while ((dir = readdir(d))) {
            if (dir->d_type == DT_REG) {
                send(client_sock, dir->d_name, strlen(dir->d_name), 0);
                send(client_sock, "\n", 1, 0);
            }
        }
        closedir(d);

    } else if (strncmp(buf, "GET ", 4) == 0) {
        char filename[256];
        sscanf(buf + 4, "%s", filename);
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", shared_dir, filename);
        send_file(path, client_sock);
    }
    close(client_sock);
}

void list_local_files() {
    DIR *d = opendir(shared_dir);
    struct dirent *dir;
    while ((dir = readdir(d))) {
        if (dir->d_type == DT_REG) {
            printf("%s\n", dir->d_name);
        }
    }
    closedir(d);
}

int send_file(const char *filename, int sock) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) return -1;
    char buf[BUFFER_SIZE];
    int n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        send(sock, buf, n, 0);
    }
    fclose(fp);
    return 0;
}

int receive_file(const char *filename, int sock) {
    FILE *fp = fopen(filename, "wb");
    if (!fp) return -1;
    char buf[BUFFER_SIZE];
    int n;
    while ((n = recv(sock, buf, sizeof(buf), 0)) > 0) {
        fwrite(buf, 1, n, fp);
    }
    fclose(fp);
    return 0;
}
int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <listen_port> [peer_ip:port ...]\n", argv[0]);
        return 1;
    }

    listen_port = atoi(argv[1]);
    for (int i = 2; i < argc && peer_count < MAX_PEERS; i++) {
        char *sep = strchr(argv[i], ':');
        if (!sep) continue;
        *sep = '\0';
        peer_ips[peer_count] = strdup(argv[i]);
        peer_ports[peer_count] = atoi(sep + 1);
        peer_count++;
    }

    mkdir(shared_dir, 0777);
    mkdir(save_dir, 0777);

    pthread_t listener;
    pthread_create(&listener, NULL, listener_thread, NULL);

    char cmd[256];
    while (1) {
        printf("\n> ");
        fgets(cmd, sizeof(cmd), stdin);
        trim_newline(cmd);

        if (strncmp(cmd, "share ", 6) == 0) {
            char filename[256];
            sscanf(cmd + 6, "%s", filename);
            char buf[512];
            snprintf(buf, sizeof(buf), "cp %s %s/", filename, shared_dir);
            system(buf);
            printf("Shared %s\n", filename);

        } else if (strncmp(cmd, "list", 4) == 0) {
            printf("Local files:\n");
            list_local_files();
            printf("\nRemote files:\n");

            fd_set readfds;
            int max_fd = -1;
            int socks[MAX_PEERS];
            for (int i = 0; i < peer_count; i++) {
                socks[i] = socket(AF_INET, SOCK_STREAM, 0);
                struct sockaddr_in peer_addr = {0};
                peer_addr.sin_family = AF_INET;
                peer_addr.sin_port = htons(peer_ports[i]);
                inet_pton(AF_INET, peer_ips[i], &peer_addr.sin_addr);

                if (connect(socks[i], (struct sockaddr*)&peer_addr, sizeof(peer_addr)) == 0) {
                    send(socks[i], "LIST\n", 5, 0);
                    set_nonblocking(socks[i]);
                } else {
                    close(socks[i]);
                    socks[i] = -1;
                }
            }

            FD_ZERO(&readfds);
            for (int i = 0; i < peer_count; i++) {
                if (socks[i] >= 0) {
                    FD_SET(socks[i], &readfds);
                    if (socks[i] > max_fd) max_fd = socks[i];
                }
            }

            struct timeval timeout = {2, 0};
            int ready = select(max_fd + 1, &readfds, NULL, NULL, &timeout);
            if (ready > 0) {
                char buf[BUFFER_SIZE];
                for (int i = 0; i < peer_count; i++) {
                    if (socks[i] >= 0 && FD_ISSET(socks[i], &readfds)) {
                        int n;
                        while ((n = recv(socks[i], buf, sizeof(buf) - 1, 0)) > 0) {
                            buf[n] = '\0';
                            printf("[%s:%d] %s", peer_ips[i], peer_ports[i], buf);
                        }
                        close(socks[i]);
                    }
                }
            } else {
                printf("No response from peers.\n");
            }

        } else if (strncmp(cmd, "download ", 9) == 0) {
            char filename[256];
            sscanf(cmd + 9, "%s", filename);
            int success = 0;
            for (int i = 0; i < peer_count && !success; i++) {
                int sock = socket(AF_INET, SOCK_STREAM, 0);
                struct sockaddr_in peer_addr = {0};
                peer_addr.sin_family = AF_INET;
                peer_addr.sin_port = htons(peer_ports[i]);
                inet_pton(AF_INET, peer_ips[i], &peer_addr.sin_addr);

                if (connect(sock, (struct sockaddr*)&peer_addr, sizeof(peer_addr)) == 0) {
                    char req[512];
                    snprintf(req, sizeof(req), "GET %s\n", filename);
                    send(sock, req, strlen(req), 0);
                    char path[512];
                    snprintf(path, sizeof(path), "%s/%s", save_dir, filename);
                    if (receive_file(path, sock) == 0) {
                        printf("Downloaded %s\n", filename);
                        success = 1;
                    }
                    close(sock);
                }
            }
            if (!success) printf("Download failed. File not found on any peer.\n");

        } else {
            printf("Commands:\n share <filename>\n download <filename>\n list\n");
        }
    }

    return 0;
}
