# P2P File Sharing System - README

## Group Members and Contributions

* **Đặng Trung Nguyên** – 
* **Đoàn Đình Khải** – 
* **Trương Đức Hiệp** – 
* **Đặng Thu Huyền** –
* **Nguyễn Trường Giang** –
* **Dương Minh Tiến** –
* **Bùi Mạnh Duy** –

## Build and Run Instructions

### Requirements

* OS: Ubuntu 20.04+ or any POSIX-compatible system
* Compiler: GCC
* Libraries: pthread, OpenSSL (for SHA-256)

### Build

```sh
make
```

### Run

Each peer should be launched in a separate terminal:

```sh
./peer
```

## Command Interface

```
share <filename>       # Share a local file
search <filename>      # Search for a file in the network
download <filename>    # Download a file from discovered peers
list                   # List local and discovered shared files
```

## Features Implemented

* Peer discovery using UDP broadcast
* Distributed file indexing
* File segmentation and chunked downloads
* Parallel multi-peer download
* Bandwidth limiting per thread
* SHA-256 file integrity verification
* CLI with live progress reporting

## Challenges and Solutions

* **Peer discovery:** Using UDP broadcast made peer detection fast, but limited it to LANs. Solution: kept it simple for demo.
* **Parallel downloading coordination:** Managed race conditions using file locks and mutexes.
* **Integrity checking:** Integrated OpenSSL SHA-256 for robust hash checking post-download.
* **Bandwidth limiting:** Implemented per-thread rate limiting with `usleep()` and timing control.

## Notes

* Ensure peers are on the same local network and firewall rules allow UDP/TCP ports.
* Use `list` command after startup to see shared files from other peers.
* Avoid duplicate filenames in the shared folder.

---


