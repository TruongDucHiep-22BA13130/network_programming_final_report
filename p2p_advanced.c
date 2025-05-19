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
#include <fcntl.h>
#include <openssl/sha.h>
#include <errno.h>
#include <time.h>

#define MAX_PEERS 32
#define MAX_FILES 256
#define CHUNK_SIZE 262144 // 256 KB
#define BUFFER_SIZE CHUNK_SIZE
#define UDP_PORT 9000
#define BROADCAST_INTERVAL 3 // seconds
#define MAX_SOURCES 8

typedef struct {
    char ip[INET_ADDRSTRLEN];
    int port;
    time_t last_seen;
} PeerInfo;

typedef struct {
    char name[256];
    unsigned char hash[SHA256_DIGEST_LENGTH];
    size_t size;
    int num_chunks;
    int num_sources;
    PeerInfo sources[MAX_SOURCES];
} FileInfo;

PeerInfo peers[MAX_PEERS];
int peer_count = 0;
FileInfo files[MAX_FILES];
int file_count = 0;
int listen_port;
char my_ip[INET_ADDRSTRLEN] = "127.0.0.1";

pthread_mutex_t peer_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

void* udp_broadcast_thread(void* arg);
void* udp_listen_thread(void* arg);
void* tcp_server_thread(void* arg);
void handle_client(int *pclient);
void handle_command();
void search_files(const char* keyword);
void download_file(int file_id);
void compute_file_hash(const char* filename, unsigned char* hash, size_t* filesize);
void index_local_files();
void get_my_ip(char *buf) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in serv = {0};
    serv.sin_family = AF_INET;
    serv.sin_addr.s_addr = inet_addr("8.8.8.8");
    serv.sin_port = htons(53);
    connect(sock, (struct sockaddr*)&serv, sizeof(serv));
    struct sockaddr_in name;
    socklen_t namelen = sizeof(name);
    getsockname(sock, (struct sockaddr*)&name, &namelen);
    inet_ntop(AF_INET, &name.sin_addr, buf, INET_ADDRSTRLEN);
    close(sock);
}

void print_hash(unsigned char *hash) {
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
        printf("%02x", hash[i]);
}

int hash_equal(unsigned char *a, unsigned char *b) {
    return memcmp(a, b, SHA256_DIGEST_LENGTH) == 0;
}

void* udp_broadcast_thread(void* arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    int broadcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    struct sockaddr_in baddr = {0};
    baddr.sin_family = AF_INET;
    baddr.sin_port = htons(UDP_PORT);
    baddr.sin_addr.s_addr = inet_addr("255.255.255.255");
    char msg[64];
    while (1) {
        snprintf(msg, sizeof(msg), "PEER %s %d", my_ip, listen_port);
        sendto(sock, msg, strlen(msg), 0, (struct sockaddr*)&baddr, sizeof(baddr));
        sleep(BROADCAST_INTERVAL);
    }
    return NULL;
}

void* udp_listen_thread(void* arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(UDP_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(sock, (struct sockaddr*)&addr, sizeof(addr));
    char buf[128];
    while (1) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf)-1, 0, (struct sockaddr*)&src, &slen);
        if (n > 0) {
            buf[n] = 0;
            char ip[INET_ADDRSTRLEN];
            int port;
            if (sscanf(buf, "PEER %15s %d", ip, &port) == 2) {
                if (strcmp(ip, my_ip) == 0 && port == listen_port) continue;
                pthread_mutex_lock(&peer_mutex);
                int found = 0;
                for (int i = 0; i < peer_count; i++) {
                    if (strcmp(peers[i].ip, ip) == 0 && peers[i].port == port) {
                        peers[i].last_seen = time(NULL);
                        found = 1;
                        break;
                    }
                }
                if (!found && peer_count < MAX_PEERS) {
                    strcpy(peers[peer_count].ip, ip);
                    peers[peer_count].port = port;
                    peers[peer_count].last_seen = time(NULL);
                    peer_count++;
                    printf("[DISCOVERY] New peer: %s:%d\n", ip, port);
                }
                pthread_mutex_unlock(&peer_mutex);
            }
        }
    }
    return NULL;
}

void compute_file_hash(const char* filename, unsigned char* hash, size_t* filesize) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) return;
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    char buf[BUFFER_SIZE];
    size_t n, total = 0;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        SHA256_Update(&ctx, buf, n);
        total += n;
    }
    SHA256_Final(hash, &ctx);
    if (filesize) *filesize = total;
    fclose(fp);
}

void index_local_files() {
    DIR *d = opendir("shared");
    struct dirent *dir;
    file_count = 0;
    while ((dir = readdir(d))) {
        if (dir->d_type == DT_REG && file_count < MAX_FILES) {
            char path[512];
            snprintf(path, sizeof(path), "shared/%s", dir->d_name);
            unsigned char hash[SHA256_DIGEST_LENGTH];
            size_t size;
            compute_file_hash(path, hash, &size);
            strcpy(files[file_count].name, dir->d_name);
            memcpy(files[file_count].hash, hash, SHA256_DIGEST_LENGTH);
            files[file_count].size = size;
            files[file_count].num_chunks = (size + CHUNK_SIZE - 1) / CHUNK_SIZE;
            files[file_count].num_sources = 1;
            strcpy(files[file_count].sources[0].ip, my_ip);
            files[file_count].sources[0].port = listen_port;
            file_count++;
        }
    }
    closedir(d);
}

void* tcp_server_thread(void* arg) {
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(listen_port);
    bind(server_sock, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_sock, 8);
    while (1) {
        int client_sock = accept(server_sock, NULL, NULL);
        if (client_sock < 0) continue;
        pthread_t t;
        int *pclient = malloc(sizeof(int));
        *pclient = client_sock;
        pthread_create(&t, NULL, (void*(*)(void*))handle_client, pclient);
        pthread_detach(t);
    }
    return NULL;
}

void handle_client(int *pclient) {
    int sock = *pclient;
    free(pclient);
    char buf[BUFFER_SIZE];
    int n = recv(sock, buf, sizeof(buf)-1, 0);
    if (n <= 0) { close(sock); return; }
    buf[n] = 0;
    if (strncmp(buf, "SEARCH ", 7) == 0) {
        char keyword[128];
        sscanf(buf+7, "%127s", keyword);
        pthread_mutex_lock(&file_mutex);
        for (int i = 0; i < file_count; i++) {
            if (strstr(files[i].name, keyword)) {
                char msg[512];
                char hashstr[65];
                for (int j = 0; j < SHA256_DIGEST_LENGTH; j++)
                    sprintf(hashstr+2*j, "%02x", files[i].hash[j]);
                snprintf(msg, sizeof(msg), "FOUND %s %zu %d %s %d\n", files[i].name, files[i].size, files[i].num_chunks, hashstr, listen_port);
                send(sock, msg, strlen(msg), 0);
            }
        }
        pthread_mutex_unlock(&file_mutex);
    } else if (strncmp(buf, "GET ", 4) == 0) {
        char fname[256];
        int chunk;
        sscanf(buf+4, "%255s %d", fname, &chunk);
        char path[512];
        snprintf(path, sizeof(path), "shared/%s", fname);
        FILE *fp = fopen(path, "rb");
        if (!fp) { close(sock); return; }
        fseek(fp, chunk*CHUNK_SIZE, SEEK_SET);
        char chunkbuf[CHUNK_SIZE];
        int tosend = fread(chunkbuf, 1, CHUNK_SIZE, fp);
        fclose(fp);
        send(sock, chunkbuf, tosend, 0);
    }
    close(sock);
}

void handle_command() {
    char cmd[256];
    while (1) {
        printf("\n> ");
        fflush(stdout);
        if (!fgets(cmd, sizeof(cmd), stdin)) break;
        if (strncmp(cmd, "share ", 6) == 0) {
            char filename[256];
            sscanf(cmd+6, "%255s", filename);
            char dest[512];
            snprintf(dest, sizeof(dest), "shared/%s", strrchr(filename, '/') ? strrchr(filename, '/')+1 : filename);
            if (access(filename, F_OK) == 0) {
                char syscmd[1024];
                snprintf(syscmd, sizeof(syscmd), "cp '%s' '%s'", filename, dest);
                system(syscmd);
                index_local_files();
                printf("Shared %s\nHash: ", dest+7);
                print_hash(files[file_count-1].hash);
                printf("\n");
            } else {
                printf("File not found\n");
            }
        } else if (strncmp(cmd, "list", 4) == 0) {
            index_local_files();
            printf("Local files:\n");
            pthread_mutex_lock(&file_mutex);
            for (int i = 0; i < file_count; i++) {
                printf("%d. %s (%.2f MB) ", i+1, files[i].name, files[i].size/1024.0/1024.0);
                print_hash(files[i].hash);
                printf("\n");
            }
            pthread_mutex_unlock(&file_mutex);
        } else if (strncmp(cmd, "search ", 7) == 0) {
            char keyword[128];
            sscanf(cmd+7, "%127s", keyword);
            search_files(keyword);
        } else if (strncmp(cmd, "download ", 9) == 0) {
            int id;
            sscanf(cmd+9, "%d", &id);
            download_file(id-1);
        } else {
            printf("Commands:\n share <filename>\n search <keyword>\n download <file_id>\n list\n");
        }
    }
}

void search_files(const char* keyword) {
    printf("Searching for '%s'...\n", keyword);
    pthread_mutex_lock(&peer_mutex);
    int n_peers = peer_count;
    PeerInfo peerlist[MAX_PEERS];
    memcpy(peerlist, peers, sizeof(peerlist));
    pthread_mutex_unlock(&peer_mutex);
    pthread_mutex_lock(&file_mutex);
    index_local_files();
    pthread_mutex_unlock(&file_mutex);
    int found = 0;
    for (int i = 0; i < n_peers; i++) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(peerlist[i].port);
        inet_pton(AF_INET, peerlist[i].ip, &addr.sin_addr);
        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            char req[256];
            snprintf(req, sizeof(req), "SEARCH %s", keyword);
            send(sock, req, strlen(req), 0);
            char buf[1024];
            int n;
            while ((n = recv(sock, buf, sizeof(buf)-1, 0)) > 0) {
                buf[n] = 0;
                char fname[256], hashstr[65];
                size_t size; int chunks, port;
                if (sscanf(buf, "FOUND %255s %zu %d %64s %d", fname, &size, &chunks, hashstr, &port) == 5) {
                    printf("%d. %s (%.2f MB) - Source: %s:%d\n", found+1, fname, size/1024.0/1024.0, peerlist[i].ip, port);
                    found++;
                }
            }
        }
        close(sock);
    }
    if (!found) printf("No results found.\n");
}

typedef struct {
    char fname[256];
    unsigned char hash[SHA256_DIGEST_LENGTH];
    size_t size;
    int chunk_idx;
    char ip[INET_ADDRSTRLEN];
    int port;
    char *chunkbuf;
    int *chunk_lens;
    int *done;
} ChunkTask;

void* download_chunk_thread(void* arg) {
    ChunkTask *task = (ChunkTask*)arg;
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(task->port);
    inet_pton(AF_INET, task->ip, &addr.sin_addr);
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        char req[512];
        snprintf(req, sizeof(req), "GET %s %d", task->fname, task->chunk_idx);
        send(sock, req, strlen(req), 0);
        int received = 0;
        while (received < CHUNK_SIZE) {
           int n = recv(sock, task->chunkbuf + task->chunk_idx * CHUNK_SIZE + received, CHUNK_SIZE - received, 0);
           if (n <= 0) break;
           received += n;
        }

        if (received > 0) task->chunk_lens[task->chunk_idx] = received;
        task->done[task->chunk_idx] = 1;
    }
    close(sock);
    return NULL;
}

void download_file(int file_id) {
    pthread_mutex_lock(&file_mutex);
    if (file_id < 0 || file_id >= file_count) {
        pthread_mutex_unlock(&file_mutex);
        printf("Invalid file id\n");
        return;
    }
    FileInfo f = files[file_id];
    pthread_mutex_unlock(&file_mutex);
    printf("Downloading '%s' (%.2f MB, %d chunks)...\n", f.name, f.size/1024.0/1024.0, f.num_chunks);
    char *filebuf = malloc(f.num_chunks*CHUNK_SIZE);
    int *chunk_lens = calloc(f.num_chunks, sizeof(int));
    int *done = calloc(f.num_chunks, sizeof(int));
    pthread_t threads[f.num_chunks];
    ChunkTask tasks[f.num_chunks];
    for (int i = 0; i < f.num_chunks; i++) {
        strcpy(tasks[i].fname, f.name);
        memcpy(tasks[i].hash, f.hash, SHA256_DIGEST_LENGTH);
        tasks[i].size = f.size;
        tasks[i].chunk_idx = i;
        
        pthread_mutex_lock(&peer_mutex);
        if (peer_count > 0) {
            strcpy(tasks[i].ip, peers[0].ip);
            tasks[i].port = peers[0].port;
        } else {
            strcpy(tasks[i].ip, my_ip);
            tasks[i].port = listen_port;
        }
        pthread_mutex_unlock(&peer_mutex);
        tasks[i].chunkbuf = filebuf;
        tasks[i].chunk_lens = chunk_lens;
        tasks[i].done = done;
        pthread_create(&threads[i], NULL, download_chunk_thread, &tasks[i]);
    }

    int completed = 0;
    while (completed < f.num_chunks) {
        completed = 0;
        for (int i = 0; i < f.num_chunks; i++) completed += done[i];
        printf("\r[%-20s] %3d%%", 
            (char[21]){[0 ... 19] = '=', 0}, (completed*100)/f.num_chunks);
        fflush(stdout);
        usleep(200000);
    }
    printf("\nMerging chunks...\n");
    char outpath[512];
    snprintf(outpath, sizeof(outpath), "downloads/%s", f.name);
    FILE *fp = fopen(outpath, "wb");
    for (int i = 0; i < f.num_chunks; i++) {
        fwrite(filebuf + i*CHUNK_SIZE, 1, chunk_lens[i], fp);
    }
    fclose(fp);

    unsigned char hash[SHA256_DIGEST_LENGTH];
    size_t sz;
    compute_file_hash(outpath, hash, &sz);
    if (hash_equal(hash, f.hash)) {
        printf("Download complete. File saved to %s\n", outpath);
    } else {
        printf("Hash mismatch! Download corrupted.\n");
    }
    free(filebuf);
    free(chunk_lens);
    free(done);
}


int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: %s <listen_port>\n", argv[0]);
        return 1;
    }
    listen_port = atoi(argv[1]);
    mkdir("shared", 0777);
    mkdir("downloads", 0777);
    get_my_ip(my_ip);
    index_local_files();
    pthread_t udp_bcast, udp_listen, tcp_server;
    pthread_create(&udp_bcast, NULL, udp_broadcast_thread, NULL);
    pthread_create(&udp_listen, NULL, udp_listen_thread, NULL);
    pthread_create(&tcp_server, NULL, tcp_server_thread, NULL);
    handle_command();
    return 0;
} 
