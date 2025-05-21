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
#include <sys/time.h>

#define MAX_PEERS 32
#define MAX_FILES 256
#define CHUNK_SIZE 262144 
#define BUFFER_SIZE 4096
#define UDP_PORT 9000
#define BROADCAST_INTERVAL 3 
#define MAX_SOURCES 8
#define RATE_LIMIT 100*1024
#define MAX_REMOTE_FILES 512


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


typedef struct {
    char name[256];
    size_t size;
    int num_chunks;
    unsigned char hash[SHA256_DIGEST_LENGTH];
    PeerInfo sources[MAX_SOURCES];
    int num_sources;
} RemoteFile;

PeerInfo peers[MAX_PEERS];
int peer_count = 0;
FileInfo files[MAX_FILES];
int file_count = 0;
int listen_port;
char my_ip[INET_ADDRSTRLEN] = "127.0.0.1";

pthread_mutex_t peer_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

RemoteFile remote_files[MAX_REMOTE_FILES];
int remote_file_count = 0;

void* udp_broadcast_thread(void* arg);
void* udp_listen_thread(void* arg);
void* tcp_server_thread(void* arg);
void handle_client(int *pclient);
void handle_command();
void fetch_remote_files(const char* keyword, int silent);
void download_file(int file_id);
void compute_file_hash(const char* filename, unsigned char* hash, size_t* filesize);
void index_local_files();
void list_current_dir_files();


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
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(UDP_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind UDP failed");
        exit(1);
    }
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
    if (strncmp(buf, "SEARCH", 6) == 0) {
        char keyword[128] = "";
        if (strlen(buf) > 6) {
            sscanf(buf+7, "%127s", keyword);
        }
        pthread_mutex_lock(&file_mutex);
        for (int i = 0; i < file_count; i++) {
            if (strlen(keyword) == 0 || strstr(files[i].name, keyword)) {
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
                int found = 0;
                for (int i = 0; i < file_count; i++) {
                    if (strcmp(files[i].name, dest+7) == 0) {
                        printf("Shared %s\nHash: ", dest+7);
                        print_hash(files[i].hash);
                        printf("\n");
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    printf("Shared %s, but could not find hash!\n", dest+7);
                }
            } else {
                printf("File not found\n");
            }
        } else if (strncmp(cmd, "list", 4) == 0) {
            printf("Local files:\n");
            list_current_dir_files();
            printf("Shared files:\n");
            index_local_files();
            pthread_mutex_lock(&file_mutex);
            for (int i = 0; i < file_count; i++) {
                printf("%d. %s (%.2f MB) ", i+1, files[i].name, files[i].size/1024.0/1024.0);
                print_hash(files[i].hash);
                printf("\n");
            }
            pthread_mutex_unlock(&file_mutex);
            printf("Remote files:\n");
            fetch_remote_files("", 1);
            for (int i = 0; i < remote_file_count; i++) {
                printf("%d. %s (%.2f MB) - Source: %s:%d\n", i + 1, remote_files[i].name, remote_files[i].size/1024.0/1024.0, remote_files[i].sources[0].ip, remote_files[i].sources[0].port);
            }
            if (remote_file_count == 0) printf("No remote files found.\n");
        } else if (strncmp(cmd, "search ", 7) == 0) {
            char keyword[128];
            sscanf(cmd+7, "%127s", keyword);
            printf("Searching for '%s'...\n", keyword);
            fetch_remote_files(keyword, 0);
        } else if (strncmp(cmd, "download ", 9) == 0) {
            int id;
            sscanf(cmd+9, "%d", &id);
            download_file(id-1);
        } else {
            printf("Commands:\n share <filename>\n search <keyword>\n download <file_id>\n list\n");
        }
    }
}

void fetch_remote_files(const char* keyword, int silent) {
    remote_file_count = 0;
    pthread_mutex_lock(&peer_mutex);
    int n_peers = peer_count;
    PeerInfo peerlist[MAX_PEERS];
    memcpy(peerlist, peers, sizeof(peerlist));
    pthread_mutex_unlock(&peer_mutex);
    int found = 0;
    for (int i = 0; i < n_peers; i++) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        struct timeval tv;
        tv.tv_sec = 2;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
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
                    unsigned char hash[SHA256_DIGEST_LENGTH];
                    for (int j = 0; j < SHA256_DIGEST_LENGTH; j++)
                        sscanf(hashstr + 2*j, "%2hhx", &hash[j]);
                    int idx = -1;
                    for (int k = 0; k < remote_file_count; k++) {
                        if (memcmp(remote_files[k].hash, hash, SHA256_DIGEST_LENGTH) == 0) {
                            idx = k;
                            break;
                        }
                    }
                    if (idx == -1 && remote_file_count < MAX_REMOTE_FILES) {
                        idx = remote_file_count++;
                        strncpy(remote_files[idx].name, fname, 255);
                        remote_files[idx].size = size;
                        remote_files[idx].num_chunks = chunks;
                        memcpy(remote_files[idx].hash, hash, SHA256_DIGEST_LENGTH);
                        remote_files[idx].num_sources = 0;
                        strcpy(remote_files[idx].sources[0].ip, peerlist[i].ip);
                        remote_files[idx].sources[0].port = port;
                        remote_files[idx].sources[0].last_seen = time(NULL);
                        remote_files[idx].num_sources = 1;
                        if (!silent) {
                            printf("%d. %s (%.2f MB) - 1 sources\n", idx+1, fname, size/1024.0/1024.0);
                        }
                    } else if (idx != -1) {
                        int already = 0;
                        for (int s = 0; s < remote_files[idx].num_sources; s++) {
                            if (strcmp(remote_files[idx].sources[s].ip, peerlist[i].ip) == 0 &&
                                remote_files[idx].sources[s].port == port) {
                                already = 1;
                                break;
                            }
                        }
                        if (!already && remote_files[idx].num_sources < MAX_SOURCES) {
                            strcpy(remote_files[idx].sources[remote_files[idx].num_sources].ip, peerlist[i].ip);
                            remote_files[idx].sources[remote_files[idx].num_sources].port = port;
                            remote_files[idx].sources[remote_files[idx].num_sources].last_seen = time(NULL);
                            remote_files[idx].num_sources++;
                        }
                    }
                    found++;
                }
            }
        }
        close(sock);
    }
    if (!silent && found == 0) printf("No results found.\n");
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
    int result;
} ChunkTask;

void* download_chunk_thread(void* arg) {
    ChunkTask *task = (ChunkTask*)arg;
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(task->port);
    inet_pton(AF_INET, task->ip, &addr.sin_addr);
    task->result = 0; 
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        char req[512];
        snprintf(req, sizeof(req), "GET %s %d", task->fname, task->chunk_idx);
        send(sock, req, strlen(req), 0);
        size_t total = 0;
        size_t expected = CHUNK_SIZE;
        if (task->chunk_idx == (task->size + CHUNK_SIZE - 1) / CHUNK_SIZE - 1) {
            expected = task->size - (task->chunk_idx * CHUNK_SIZE);
        }
        char *chunkbuf = malloc(expected);
        int n;
        while (total < expected && (n = recv(sock, chunkbuf + total, expected - total, 0)) > 0) {
            total += n;
        }
        if (total == expected) {
            FILE *fp = fopen(task->fname, "r+b");
            if (!fp) fp = fopen(task->fname, "w+b");
            if (fp) {
                fseek(fp, task->chunk_idx * CHUNK_SIZE, SEEK_SET);
                fwrite(chunkbuf, 1, total, fp);
                fclose(fp);
                task->chunk_lens[task->chunk_idx] = total;
                task->done[task->chunk_idx] = 1;
                task->result = 1; 
            }
        } else {
            printf("Chunk %d: expected %zu bytes, got %zu bytes from %s:%d\n",
                task->chunk_idx, expected, total, task->ip, task->port);
        }
        free(chunkbuf);
    }
    close(sock);
    return NULL;
}

void download_file(int file_id) {
    if (file_id < 0 || file_id >= remote_file_count) {
        printf("Invalid remote file id\n");
        return;
    }
    RemoteFile *rf = &remote_files[file_id];
    printf("Downloading '%s' from %d sources (%.2f MB, %d chunks)...\n", rf->name, rf->num_sources, rf->size/1024.0/1024.0, rf->num_chunks);
    int *chunk_lens = calloc(rf->num_chunks, sizeof(int));
    int *done = calloc(rf->num_chunks, sizeof(int));
    char outpath[512];
    snprintf(outpath, sizeof(outpath), "downloads/%s", rf->name);

    FILE *fp_init = fopen(outpath, "wb");
    if (fp_init) {
        fseek(fp_init, rf->size - 1, SEEK_SET);
        fputc(0, fp_init);
        fclose(fp_init);
    }

    if (rf->num_sources == 1) {

        for (int i = 0; i < rf->num_chunks; i++) {
            int sock = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in addr = {0};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(rf->sources[0].port);
            inet_pton(AF_INET, rf->sources[0].ip, &addr.sin_addr);
            if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                char req[512];
                snprintf(req, sizeof(req), "GET %s %d", rf->name, i);
                send(sock, req, strlen(req), 0);
                size_t total = 0;
                size_t expected = CHUNK_SIZE;
                if (i == (rf->size + CHUNK_SIZE - 1) / CHUNK_SIZE - 1) {
                    expected = rf->size - (i * CHUNK_SIZE);
                }
                char *chunkbuf = malloc(expected);
                int n;
                while (total < expected && (n = recv(sock, chunkbuf + total, expected - total, 0)) > 0) {
                    total += n;
                }
                if (total == expected) {
                    FILE *fp = fopen(outpath, "r+b");
                    if (!fp) fp = fopen(outpath, "w+b");
                    if (fp) {
                        fseek(fp, i * CHUNK_SIZE, SEEK_SET);
                        fwrite(chunkbuf, 1, total, fp);
                        fclose(fp);
                        chunk_lens[i] = total;
                        done[i] = 1;
                    }
                } else {
                    printf("Chunk %d failed from %s:%d. Download aborted.\n", i, rf->sources[0].ip, rf->sources[0].port);
                    close(sock);
                    free(chunkbuf); free(chunk_lens); free(done);
                    return;
                }
                free(chunkbuf);
            } else {
                printf("Connect failed to %s:%d. Download aborted.\n", rf->sources[0].ip, rf->sources[0].port);
                free(chunk_lens); free(done);
                return;
            }
            close(sock);
            printf("\r[%-20s] %3d%%", (char[21]){[0 ... 19] = '=', 0}, ((i+1)*100)/rf->num_chunks);
            fflush(stdout);
        }
        printf("\nMerging chunks...\n");
    } else {

        int max_retry = 3;
        int *last_src = calloc(rf->num_chunks, sizeof(int));
        for (int i = 0; i < rf->num_chunks; i++) last_src[i] = -1;
        for (int attempt = 0; attempt < max_retry; attempt++) {
            pthread_t threads[rf->num_chunks];
            ChunkTask tasks[rf->num_chunks];
            int need_download = 0;
            for (int i = 0; i < rf->num_chunks; i++) {
                if (done[i]) continue; 
                int src;
                do {
                    src = rand() % rf->num_sources;
                } while (rf->num_sources > 1 && src == last_src[i]);
                last_src[i] = src;
                strcpy(tasks[i].fname, outpath);
                memcpy(tasks[i].hash, rf->hash, SHA256_DIGEST_LENGTH);
                tasks[i].size = rf->size;
                tasks[i].chunk_idx = i;
                strcpy(tasks[i].ip, rf->sources[src].ip);
                tasks[i].port = rf->sources[src].port;
                tasks[i].chunkbuf = NULL; 
                tasks[i].chunk_lens = chunk_lens;
                tasks[i].done = done;
                tasks[i].result = 0;
                pthread_create(&threads[i], NULL, download_chunk_thread, &tasks[i]);
                need_download = 1;
            }
            if (!need_download) break; 

            for (int i = 0; i < rf->num_chunks; i++) {
                if (done[i]) continue;
                pthread_join(threads[i], NULL);
                if (!tasks[i].result) {
                    printf("Chunk %d failed from %s:%d, retrying...\n", i, tasks[i].ip, tasks[i].port);
                }
            }

            int completed = 0;
            for (int i = 0; i < rf->num_chunks; i++) completed += done[i];
            printf("\r[%-20s] %3d%%", (char[21]){[0 ... 19] = '=', 0}, (completed*100)/rf->num_chunks);
            fflush(stdout);
            if (completed == rf->num_chunks) break;
        }
        free(last_src);
        printf("\nMerging chunks...\n");

        int failed = 0;
        for (int i = 0; i < rf->num_chunks; i++) {
            if (!done[i]) {
                printf("Chunk %d failed after retries. Download aborted.\n", i);
                failed = 1;
            }
        }
        if (failed) {
            free(chunk_lens); free(done);
            return;
        }
    }

    unsigned char hash[SHA256_DIGEST_LENGTH];
    size_t sz;
    compute_file_hash(outpath, hash, &sz);
    if (hash_equal(hash, rf->hash)) {
        printf("Download complete. File saved to %s\n", outpath);
    } else {
        printf("Hash mismatch! Download corrupted.\n");
    }
    free(chunk_lens);
    free(done);
}


void list_current_dir_files() {
    DIR *d = opendir(".");
    struct dirent *dir;
    int idx = 1;
    while ((dir = readdir(d))) {
        if (dir->d_type == DT_REG) {
            printf("%d. %s\n", idx++, dir->d_name);
        }
    }
    closedir(d);
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
