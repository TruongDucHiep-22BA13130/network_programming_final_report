# network_programming_final_report
Peer-to-peer file sharing system
P2P file sharing with search, segmented downloads, and parallel transfer.
CLI: search lecture_notes.pdf
Terminal prints:
 Found at peers:
 1. 192.168.1.2
 2. 192.168.1.4
 Downloading from 2 peers... Done.

Directory : 
p2p/
├── final_0.0.c
├── shared/
│   ├── file1.txt
│   └── ...
Run instruction :

terminal 0:
./final_0.0 5000 127.0.0.1:5001 127.0.0.1:5002
share file.txt
list
download file.txt

terminal 1:
./final_0.0 5001 127.0.0.1:5000 127.0.0.1:5002
share file.txt
list
download file.txt

terminal 2:
./final_0.0 5002 127.0.0.1:5000 127.0.0.1:5001
share file.txt
list
download file.txt
