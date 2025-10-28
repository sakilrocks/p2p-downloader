# p2p-downloader

a lightweight, high performance peer to peer file downloader written in C++17, using threads, libcurl, and sockets for concurrent downloads.
each peer broadcasts its shared files on the network, discovers other peers automatically, and downloads files in parallel chunks from multiple sources.

---

## Features
```
	•	 Peer Discovery: Automatic LAN broadcast to find active peers.
	•	 Concurrent Downloads: multi-threaded file fetching using std::thread and libcurl.
	•	 Chunked Transfers: Large files are split into parts and fetched in parallel.
	•	 TCP File Sharing: Each peer hosts its own TCP server to serve requested files.
	•	 Thread safe Peer Management: Uses std::mutex and atomic flags for safe concurrent access.
	•	 Cross platform: Works on Linux and macOS (requires minor tweaks for Windows).
```

---

## Project Structure

```
├── include/
│   ├── network.hpp      # peer discovery + TCP server
│   ├── downloader.hpp   # handles threaded downloads
│   ├── utils.hpp        # string utilities
├── src/
│   ├── main.cpp         # CLI entry point
│   ├── network.cpp      # peer networking logic
│   ├── downloader.cpp   # multi-threaded file downloading
│   ├── utils.cpp        # utility function definitions
├── Makefile
└── README.md
```

---

How it works:
```
	1.	Broadcast: Each peer periodically announces its presence and shared files via UDP broadcast.
	2.	Discovery: Other peers receive these broadcasts and maintain a list of available peers.
	3.	Download: When a file is requested, the downloader connects to multiple peers concurrently
             and fetches different chunks.
	4.	Assembly: Downloaded chunks are merged into the final file.
```

---

## Build Instructions

Dependencies
```
	•	  C++17 compiler (GCC / Clang)  
	•	  libcurl (sudo apt install libcurl4-openssl-dev)  
	•	  Make  
```
Run
```
./p2p_downloader <shared_folder> <service_port>
```

---


