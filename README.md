# P2P File Sharing System - README

## Group Members and Contributions

| Member                   | Role                                          | Job description                                                                                                                       
| ----------------         | --------------------------------------------- | -----------------------------------------------------------------------------------------------------------------------------------|
| **Trương Dức Hiệp**      **System leader / architecture**                 Overall architectural design P2P <br>- Coordinating modules <br>- Final integrated inspection            
| **Nguyễn Trường Giang**  **Peer Discovery & UDP protocol**                Install the Peer Discovery Settings via UDP Broadcast <br>- Periodic processing and receiving peer information <br>- Update the Peer                                                                               list of activities
| **Dương Minh Tiến**      **TCP Server & Query File**                      Deploying TCP server receives the request for search and sharing <br>- Handling multiple clients simultaneously with Multithreading                 
| **Đoàn Đình Khải**       **Search & Communication Peer**                  Handling search queries from users <br>- Send the request to the peer, synthesize feedback <br>- Display search results for users   
| **Đặng Thu Huyền**       **Download file & protocol chunk**               Settings file downloads in partial parts (chunk) <br>- Multi-load management simultaneously from many peer <br>- combine files and                                                                                 check the integrity      
| **Bùi Mạnh Duy**         **SHA-256 & Share file management**              Install the hash code sha-256 <br>- Browse the folder `shared/`, create the file index <br>- Compare the hash code after downloading                         
| **Đặng Trung Nguyên**   **Command line interface (CLI) & Documentation** Design and process commands 'share`, ``, download', `list` <br>- Communicate with users <br>- Write Readme, Instructions for use and                                                                               report

## Build and Run Instructions

### Requirements

* OS: Ubuntu 20.04+ or any POSIX-compatible system
* Compiler: GCC
* Libraries: pthread, OpenSSL (for SHA-256)

### Build

```sh
make clean && make
```

### Run

Each peer should be launched in a separate terminal:

```sh
./p2p_advanced
```

## Command Interface

```
share <filename>       # Share a local file
search <filename>      # Search for a file in the network
download <filename>    # Download a file from discovered peers
list                   # List local and discovered shared files
```

## Overview of main functions
Peer Discovery (Search for peer -to -peer machines):
Use UDP Broadcast to notify Peer's existence to other peer.
Get broadcast from other peer and save new peer information.
TCP Server:
Each peer opens a TCP server to accept search requirements and download files.
File indexing:
Prepare the file index in Shared/ to share with other peers.
Search:
Allows users to search files from other peer via TCP.
Download:
Download the file by dividing the file into "chunk" and downloading parallel from a peer (or can be expanded into multiple sources).
Cli Interface (command line interface):
Share, list, search, download.

## Core technique is used
Technology features use
Peer Discovery UDP Broadcast (SO_BROADCAST)
TCP SOCKET Data (Send, Recv) data
Share file hashing (sha256) + chunking
Multi -threaded pthread_create, pthread_mutex
Folders & File Opendir, Readdir, Fopen
MUTEX synchronous data to protect Peers [], files []

## Request directory
Shared/ - The place to store sharing files.

Downloads/ - Where to save the file after downloading.

## Features Implemented

* Peer discovery using UDP broadcast
* Distributed file indexing
* File segmentation and chunked downloads
* Parallel multi-peer download
* Bandwidth limiting per thread
* SHA-256 file integrity verification
* CLI with live progress reporting

##  Challenges Encountered and Solutions
1. Peer Discovery Reliability
Challenge: UDP broadcasts were not always received due to firewall or socket options.
Solution: Enabled socket options SO_BROADCAST and SO_REUSEADDR, and ensured the firewall allowed UDP on the chosen port.
2. Concurrent Access to Shared Structures
Challenge: Race conditions when accessing shared peers[] and files[] from multiple threads.
Solution: Used pthread_mutex_t locks to protect critical sections.
3. Chunk Integrity and Merging
Challenge: Mixing up chunks or failing to reassemble the file correctly after download.
Solution: Created chunk files with unique suffixes (filename.chunkN) and ensured chunks were written in correct order before merging.
4. TCP Message Parsing
Challenge: Misinterpreting TCP messages due to incomplete recv() reads.
Solution: Used fixed-size buffer reading and ensured full message receipt before processing.
5. Hashing Accuracy
Challenge: Files were downloaded but marked as corrupted due to hash mismatch.
Solution: Standardized file reading mode (rb) and ensured chunk reassembly produced exact byte-for-byte copies.
6. File Overwrites
Challenge: Downloaded files sometimes overwrote existing ones.
Solution: Added logic to rename conflicting files or warn the user.

## Notes

* Ensure peers are on the same local network and firewall rules allow UDP/TCP ports.
* Use `list` command after startup to see shared files from other peers.
* Avoid duplicate filenames in the shared folder.

---


