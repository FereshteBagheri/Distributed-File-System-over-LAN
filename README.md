# Distributed File System over Local Area Network (DFS-over-LAN)

This project implements a distributed file system that allows clients to store and retrieve files across a local area network (LAN) using a combination of TCP and UDP protocols.

The general flow of operations is as follows:

1. **Client** connects to a **Master Server** over TCP.
2. The **Master Server** allocates chunks and provides metadata about where to store or retrieve files.
3. The **Client** uses UDP to communicate with **Chunk Servers** for uploading and downloading file chunks.
4. **Chunk Servers** handle the actual storage and retrieval of file data.

| ![Architecture 1](pictures/1.png) | ![Architecture 2](pictures/2.png) | ![Architecture 3](pictures/3.png) |
| :-------------------------------: | :-------------------------------: | :-------------------------------: |

Computer Networks course project (University of Tehran), built by a team of two:
[Shahriar-0](https://github.com/Shahriar-0) and [Fereshteh Bagheri](https://github.com/FereshteBagheri).

**At a glance**

- One master server (TCP, default port `4000`) keeps the file → chunk metadata and saves it to `master_log.json`, so the metadata survives a restart.
- 15 chunk servers (UDP ports `5000`–`5014`) run on the local machine and store chunks on disk.
- A Qt Quick (QML) client splits a file into 8 KB chunks and uploads or downloads them **one chunk at a time**: each chunk is sent after the previous one is acknowledged.
- **Error correction:** every chunk is sent with Reed–Solomon parity (RS(255,223), via the bundled [schifra](DFS-over-LAN/client-gui/schifra/) library) and a CRC32. The receiver corrects up to 16 corrupted bytes in every 255-byte block and detects chunks it cannot repair.
- **Simulated noise:** the chunk servers flip random bits in every chunk they receive or send (bit error rate set with `--noise`, default `0.001`), so the error correction is exercised on every transfer.
- **Retransmission and re-routing:** the client resends a chunk when it arrives corrupted or when no reply comes within 1 second. After 5 failed attempts it asks the master to move the chunk to another chunk server.

## Build and Run

Requires Qt 6 (Core, Network, Quick) and CMake 3.16 or newer.

```bash
cd DFS-over-LAN
cmake -B build
cmake --build build
./build/master/master -p 4000          # 1. master server
./build/chunk/chunk_server             # 2. starts all 15 chunk servers
./build/client-gui/appclient-gui       # 3. client GUI (connects to 127.0.0.1:4000)
```

Options:

- `chunk_server --noise <rate>` sets the simulated bit error rate. With `0` the channel is clean; up to about `0.002` every chunk is corrected on the first try; at `0.005` roughly two chunks in three need to be sent again; from about `0.008` almost no chunk can be repaired and uploads fail after re-routing.
- `DFS_HOLE_PUNCH=1 ./build/client-gui/appclient-gui` makes the client send a `PING` to each chunk server before its first real request, to open a path through a NAT (the chunk server answers with `PONG`).

## Report

### Client

#### 1. Connection Setup

- **Constructor / Destructor**

  - Opens a `QTcpSocket` to a master server and hooks up slots for connect/disconnect, incoming data, and errors (for master).
  - Binds a `QUdpSocket` on a random port so it can both send and receive UDP datagrams (its for chunks).
  - Cleans up sockets on destruction.

- **sendCommand()**

  - Formats commands like `ALLOCATE_CHUNKS <fileId> <size>` or `LOOKUP_FILE <fileId>` and sends them via TCP.
  - On `ALLOCATE_CHUNKS`, it also opens the local file, computes its size and number of chunks, and remembers state for uploading.
  - On `LOOKUP_FILE`, it prepares an output file and state for downloading.

#### 2. Handling Master Server Replies (TCP)

- **onReadyRead()**

  - Reads lines like `OK Allocated 5 <chunkId1> <ip1> <port1> …` or `FILE_METADATA <chunkId> <ip> <port> …` from chunks.
  - Parses out the number of chunks and builds a list of `ChunkServerInfo` structs.
  - Kicks off either `uploadFileToChunk()` or `downloadFileFromChunk()` once it has the info.

#### 3. Upload Flow (UDP)

- **uploadFileToChunk()**

  - Seeks to the current chunk’s offset in the local file, reads up to 8 KB, encodes it with `encodeChunk()` (Reed–Solomon parity + CRC32; an 8 KB chunk becomes 9,435 bytes) and constructs a packet:

        ```txt
        STORE <chunkId> <encodedLength>\n
        <encoded data…>
        ```

  - Sends it as a UDP datagram to the chunk server’s IP/port (with `DFS_HOLE_PUNCH=1`, a `PING` goes first to open the NAT).
  - Starts a 1-second retry timer.
  - Emits a log signal so a UI can show “Sent chunk123 → 192.0.2.5:5001”.

- **onUdpReadyRead()** (for ACKs)

  - Reads incoming UDP datagrams, splits header and payload.
  - If header starts with `ACK`, checks that it is for the current chunk (late replies to earlier attempts are ignored), reads the chunk server’s address and the corruption flag, and emits `chunkAckReceived`.
  - If the flag says the chunk arrived corrupted, the chunk server did not store it, so the client sends it again.
  - Otherwise it records which server holds the chunk, updates the progress signal and uploads the next chunk in the list the master returned.
  - When all chunks are ACKed, emits `uploadFinished`.

- **Retries and re-routing**

  - If no `ACK` arrives within 1 second, or the chunk arrives corrupted, the chunk is sent again, up to 5 attempts.
  - After 5 failed attempts the client sends `REALLOCATE_CHUNK <fileId> <chunkId>` to the master, which assigns the chunk to the next chunk server in its DFS order and replies `OK Reallocated <chunkId> <ip> <port>`. The client then uploads the chunk there.
  - A chunk can be re-routed up to 3 times; after that the upload stops with an error.

#### 4. Download Flow (UDP)

- **downloadFileFromChunk()**

  - Sends a simple `RETRIEVE <chunkId>\n` message to the chunk server and starts the retry timer.

- **onUdpReadyRead()** (for DATA)

  - For headers beginning with `DATA`, reads the chunk ID, corruption flag, and length, and decodes the payload with `decodeChunk()`, which corrects transmission errors and checks the CRC32.
  - If the chunk could not be repaired, or no reply arrives within 1 second, it is requested again (up to 5 attempts; then the download stops with an error).
  - Otherwise it writes the data to the output file, emits progress and chunk-data signals, and requests the next chunk until done.
  - Closes the file, computes the final size/path, and emits `downloadFinished`.
  - An `ERROR <chunkId> chunk not found` reply from the chunk server stops the download.

#### 5. Signals & UI Integration

Throughout, the class emits Qt signals such as: (mostly are connected to qml)

- **connectionStateChanged()** when TCP connects/disconnects.
- **errorOccurred()** on any socket or file error.
- **uploadProgress()** and **downloadProgress()** for UI progress bars.
- **logReceived()** for simple textual logs.
- **chunkAckReceived** and **chunkDataReceived** for low‑level events.
- **uploadFinished()** and **downloadFinished()** when complete.

---

### Master

The `MasterServer` listens for TCP connections from `Client` instances, keeps track of every file’s chunk layout, and tells clients which chunk servers to contact. It:

1. Builds a binary-tree view of the 15 chunk servers and precomputes a depth-first (post-order) traversal order.
2. On each new client TCP connection, parses simple text commands (`ALLOCATE_CHUNKS`, `LOOKUP_FILE`, `REALLOCATE_CHUNK`).
3. For uploads, allocates chunk IDs and assigns them to chunk servers in a rotating DFS order.
4. For downloads, looks up stored metadata and returns the same list of `<chunkId> <ip> <port>` back to the client.
5. When a client cannot store a chunk on its assigned server, moves the chunk to the next server in DFS order (`REALLOCATE_CHUNK`).
6. Saves the metadata to `master_log.json` on shutdown (`SIGINT`) and loads it again on start.

#### 1. Networking & Signal Handling

- **Listening**

  - `startListening(address, port)` wraps `listen()` and logs success or failure.

- **New Connections**

  - On `newConnection`, grabs each `QTcpSocket*`, hooks up `readyRead` and `disconnected` signals, and tracks them in `m_clients`.

#### 2. Request Dispatch & Commands

- **onReadyRead()**

  - Reads lines like `ALLOCATE_CHUNKS 100KB.txt 123456` or `LOOKUP_FILE 100KB.txt` and calls `handleRequest()`.

- **handleRequest()**

  - Splits the incoming line on spaces and dispatches:
    - `ALLOCATE_CHUNKS <fileId> <size>` → `allocateChunks()`.
    - `LOOKUP_FILE <fileId>` → `lookupFile()`.
    - `REALLOCATE_CHUNK <fileId> <chunkId>` → `reallocateChunk()`.
    - Unknown → `ERROR Unknown command…`.

#### 3. Chunk Allocation & Lookup

- **allocateChunks()**

  - Computes `numChunks = ceil(size / CHUNK_SIZE)`.
  - Picks a random starting index in `dfsOrder` so uploads are load‑balanced across the tree.
  - For each chunk `i`, it:

        1. Creates an ID like `<fileId>_chunk_<i>`.
        2. Assigns it to the chunk server at port `dfsOrder[(startIdx + i) % dfsOrder.size()]`.

  - Stores this in `fileMetadata[fileId]`.
  - Sends back `OK Allocated <numChunks> <chunkId1> <ip1> <port1> …\n`, mirroring what the client’s `onReadyRead()` expects.

- **lookupFile()**

  - Finds the existing `FileMetadata` for `fileId`, and emits `FILE_METADATA <fileId> <chunkId1> <ip1> <port1> …\n`.
  - The client then parses that and starts its UDP download requests.

- **reallocateChunk()**

  - Finds the chunk, picks the server after its current one in `dfsOrder`, updates the metadata and replies `OK Reallocated <chunkId> <ip> <port>\n`.

#### 4. Interaction With the Client

- The client’s `sendCommand("ALLOCATE_CHUNKS", fileId, size)` leads to `MasterServer::allocateChunks()`, which decides where to store each piece.
- The client then uses that metadata to open UDP connections to the chunk servers we assigned here.
- On download, the client’s `sendCommand("LOOKUP_FILE", fileId)` triggers `lookupFile()`, letting the client pull exactly the same chunk list back down.

---

### Chunk Server

### 1. Role & Startup

Each `ChunkServer` listens on UDP port `5000 + serverId` and keeps its own `./CHUNK-<id>` folder. On `start()`, it binds that port and waits for commands from any client:

- **STORE `<chunkId>` `<length>\n<data>`**
- **RETRIEVE `<chunkId>`**
- **PING** (NAT hole-punching; answered with `PONG <port>`)

### 2. Storing a Chunk

1. **Receive STORE**: Parses the header, grabs exactly `<length>` bytes of payload.
2. **Noise and decoding**: `addNoise()` flips random bits to simulate the channel (rate set with `--noise`), then `decodeChunk()` corrects the errors with Reed–Solomon and checks the CRC32.
3. **Write to Disk**: If the chunk was recovered, saves the original data as `CHUNK-<id>/<chunkId>.bin`. If it could not be repaired, nothing is stored.
4. **Acknowledge**: Sends back

    ```txt
    ACK <chunkId> <thisIp> <thisPort> <corruptedFlag>
    ```

    so the client knows which server holds the chunk and whether it arrived intact (`corruptedFlag = 1` means it was not stored and must be sent again).

### 3. Retrieving a Chunk

1. **Receive RETRIEVE**: Reads the requested `<chunkId>`.
2. **Load & Encode**: Reads the `.bin` file, encodes it with `encodeChunk()` and adds simulated noise with `addNoise()`. (If the file does not exist, replies `ERROR <chunkId> chunk not found`.)
3. **Send DATA**: Replies with

    ```txt
    DATA <chunkId> <corruptedFlag> <length>\n<encodedData>
    ```

### 4. How It Fits with Previous Parts

- **MasterServer** told the client which chunk servers (ports) to use, based on a DFS‑shuffled list.
- **Client** reads those replies over TCP, then talks directly UDP to each `ChunkServer`.
- The simple STORE/ACK loop drives uploads; RETRIEVE/DATA drives downloads.
- Corruption flags and the simulated noise exercise the reliability code in `Client`: retransmission after a corrupted chunk or a timeout, and re-routing to another chunk server through the master.

### Error Correction (`common/rsCodec.cpp`)

- The chunk gets an 8-byte header (data length + CRC32), is split into 223-byte blocks, and every block is encoded as a 255-byte Reed–Solomon code word over GF(2⁸) with 32 parity bytes, using the schifra library. An 8 KB chunk becomes 37 code words (9,435 bytes), which still fits in one UDP datagram.
- The decoder corrects up to 16 wrong bytes in each code word, wherever they are in it.
- If a code word has more errors than that, the decoder reports a failure. In the rare case where it "corrects" a code word to the wrong data, the CRC32 no longer matches, so the chunk is still marked as corrupted rather than accepted.

## Questions

### 1. How are noises and corruptions created during transmission resolved in the network?

There are multiple ways to resolve this problem, but the most common ones are:

- Error Detection and ARQ
    Networks detect corrupted packets using error‐detecting codes like CRC (Cyclic Redundancy Check) appended to each frame, when a mismatch is detected, the receiver discards the packet and may request retransmission via ARQ protocols.
    ARQ trades additional round trip delay for reliability. it is commonly used in TCP, which ensures in‑order, error‑free delivery over unreliable links by resending lost or corrupted segments until acknowledged.

- Forward Error Correction
    FEC proactively adds redundant parity symbols to each block of data so that a limited number of bit errors can be corrected at the receiver without retransmission.
    things like include Reed–Solomon, Low‑Density Parity‑Check, and concatenated schemes (e.g., Viterbi + Reed–Solomon) for handling burst and random errors.

### 2. Video streaming platforms, due to their nature of use, utilize connectionless UDP communication. How do they manage these corruptions?

Because UDP does not retransmit lost or late packets automatically, video applications must implement their own loss‐recovery and smoothing mechanisms at the application layer. Video streaming services often integrate FEC at the application or transport layer to correct a percentage of lost or corrupted packets without waiting for retransmissions, which could introduce unacceptable lag. There are other solutions like Adaptive Bitrate Streaming to dynamically adjusts video quality based on measured packet loss and bandwidth, reducing resolution or bitrate when network quality degrades to minimize the impact of packet corruption and loss.

### 3. How are noises and corruptions at the time of deployment (information stored somewhere but may become corrupted) resolved, and what are the advantages of distributed file systems in this regard?

they are addressed via a combination of on‐disk error‐detecting codes like CRC/checksums, FEC, and replication across nodes.
Advantages of Distributed File Systems are:

- Redundant Replication & Geodiversity: Multiple replicas are stored on separate nodes/racks/data‐centers, so a single device’s silent corruption can be masked by reading a clean replica.

- Erasure Coding Across Nodes: Compared to simple replication, erasure coding reduces storage overhead while tolerating multiple failures or corruptions, reconstructing missing data from surviving shards.

- Scalability & Fault Isolation: DFS architectures localize failures, a corrupt block on one server affects only that node’s portion of the data, which can be rebuilt independently without taking the entire system offline.

### 4. Considering the parameters you have stored for noise and denoising information, on average, what percentage of sent information will be irrecoverable? What percentage of errors are undetectable?

The parameters used in the code are a bit error rate of _p_ = 10⁻³ (the default of `--noise`) and an RS(255,223) code: every 8 KB chunk is sent as 37 code words of 255 bytes, and each code word can correct up to 16 wrong bytes.

A byte is wrong if any of its 8 bits is flipped:

$$
q = 1 - (1-p)^8 \approx 7.97 × 10^{-3},
$$

so a code word has on average 255 _q_ ≈ 2 wrong bytes. It cannot be corrected if it has more than 16:

$$
P_{\rm block} = \sum_{k=17}^{255} \binom{255}{k} q^k (1-q)^{255-k} \approx 4.7 × 10^{-11},
$$

and a chunk is lost if any of its 37 code words is:

$$
P_{\rm chunk} = 1 - (1 - P_{\rm block})^{37} \approx 1.7 × 10^{-9}.
$$

So about 2 in a billion chunks would be irrecoverable on the first try, and even those are not lost: the receiver detects them and the client sends the chunk again. At _p_ = 5 × 10⁻³ the same formulas give _P_<sub>chunk</sub> ≈ 0.61, which matches what we measured (about 65% of chunks need to be sent again).

**Undetectable errors:** a chunk is accepted with wrong data only if the Reed–Solomon decoder "corrects" a code word to the wrong value **and** the CRC32 of the whole chunk still matches. The CRC32 lets such a chunk through with probability about 2⁻³² ≈ 2.3 × 10⁻¹⁰, so the share of undetectable errors is about 10⁻⁹ × 10⁻¹⁰ ≈ 10⁻¹⁹ per chunk, which is negligible.

### 5. Do distributed file systems prevent all types of data corruption? Explain the use of replicas (Replica) in these systems

No system can guard against all corruption types, especially silent bit‐flips that evade detection or catastrophic metadata loss if checksums themselves become corrupt. For absolute protection, DFS rely on background scrubbing (periodic checksum verification) and off‑site backups to mitigate very rare, undetectable errors, other methods for more minor errors are explained above.
Replicas are used for many reasons:

- Redundancy for Fault Tolerance
- Load Balancing and Availability
- Consistency Models

as they are obvious, we won't go into the details.

### 6. How can bottlenecks be eliminated using distributed file systems?

- Horizontal Scalability: By distributing data and metadata workloads across many servers, DFS avoid single‐node throughput limits. Client requests are routed to multiple chunk servers, enabling parallel reads and writes that scale with cluster size

- Client‑Side Caching and Write‑Back Models: Systems like Coda use write‐back caching, allowing client applications to proceed without immediate server interaction for metadata updates, eliminating RPC bottlenecks and smoothing load peaks

- Dynamic Load Balancing: Metadata servers can monitor server load and redirect new requests to underutilized nodes. Background rebalancing jobs migrate chunks from hot nodes to colder ones, maintaining uniform distribution over time.

### 7. Compare Content Delivery Networks (CDNs) with distributed file systems. How can these networks reduce user access time to content, and what role do distributed file systems play in this?

CDNs are globally distributed caches that store copies of web assets close to end users (edge servers), reducing network latency and improving page load times while DFS are distributed origin storage systems that provide scalable, durable, and consistent storage across data centers. CDNs intercept user requests at the network edge, serving cached content directly from the nearest edge server, which can be orders of magnitude faster than fetching from a centralized origin server. While CDNs manage geo‑distributed caching autonomously, they rely on DFS to maintain integrity and consistency of origin content.
