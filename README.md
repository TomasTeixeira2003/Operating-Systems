# TecnicoFS: Distributed File System & Message Broker

This repository contains a two-phase implementation of **TecnicoFS**, a project developed for the Operating Systems course. It evolves from a localized, thread-safe file system into a networked Message Broker system using a Client-Server architecture.

## Phase 1: The Core File System
The first phase focuses on the internal structures of a user-mode file system and the implementation of concurrency control.

### Key Features
* **I-node Structure**: Metadata management using an i-node table where each entry represents a file or directory.
* **Data Blocks**: Storage region organized into fixed-size 1KB blocks.
* **Link Management**: 
    * **Hard Links**: Multiple directory entries pointing to the same i-node.
    * **Soft Links**: Symbolic links that store the path to a target file.
* **Thread-Safety**: Implementation of fine-grained locking using `pthread_rwlock` or `pthread_mutex` to allow multiple threads to access the FS concurrently.
* **External Integration**: `tfs_copy_from_external_fs` allows importing files from the host Operating System into TecnicoFS.



---

## Phase 2: The Message Broker (Pub/Sub)
The second phase transforms the file system into a communication middleware, enabling Inter-Process Communication (IPC) via a Publish/Subscribe model.

### Key Features
* **Client-Server Model**: A central server (`mbroker`) manages all TecnicoFS operations and client connections.
* **Unix Domain Sockets**: Communication between processes is handled through local sockets for high-performance IPC.
* **Message Boxes**: Files within TecnicoFS act as "Message Boxes." 
    * **Publishers** write messages to these files.
    * **Subscribers** read messages from these files.
* **Management Utility**: A dedicated client to create, list, or delete message boxes remotely.
