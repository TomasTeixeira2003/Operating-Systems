# TecnicoFS - Simplified User-Mode File System

**TecnicoFS** is a simplified, user-mode file system implemented as a library. It provides a private instance of a file system for client processes to manage data using a POSIX-inspired API.

## Features

- **POSIX-like API**: Familiar functions for opening, closing, reading, and writing files.
- **I-node Based Structure**: Uses an i-node table to manage metadata and a data region organized in fixed-size blocks (1KB default).
- **Single-Directory Hierarchy**: Simplified structure featuring only a root directory (`/`).
- **Resource Management**: Includes allocation vectors for i-nodes and data blocks, along with a volatile open-file table.
- **Link Support**: Support for both **Hard Links** (incremental i-node counters) and **Soft Links** (symbolic paths stored in data blocks).
- **Thread-Safety**: Designed to support concurrent access from multi-threaded clients using fine-grained synchronization (`pthread_mutex` or `pthread_rwlock`).

---

## API Reference

### Core Functions
* `int tfs_init()`: Initializes the file system structures.
* `int tfs_destroy()`: Cleans up and releases file system resources.
* `int tfs_open(char const *name, int flags)`: Opens a file and returns a handle.
* `int tfs_close(int fhandle)`: Closes an open file handle.
* `ssize_t tfs_read(int fhandle, void *buffer, size_t len)`: Reads data from a file.
* `ssize_t tfs_write(int fhandle, void const *buffer, size_t len)`: Writes data to a file.

### Extended Functionalities
* `int tfs_copy_from_external_fs(char const *source, char const *dest)`: Imports a file from the host OS into TecnicoFS.
* `int tfs_link(char const *target, char const *source)`: Creates a hard link to an existing file.
* `int tfs_sym_link(char const *target, char const *source)`: Creates a symbolic (soft) link.
* `int tfs_unlink(char const *target)`: Removes a link or file; deletes the file if the hard link count reaches zero.

---

## System Architecture



The system state is divided into:
1.  **I-node Table**: Global table where each entry (i-node) represents a file or directory.
2.  **Data Region**: Fixed-size blocks containing actual file content or directory tables.
3.  **Allocation Vectors**: Bitmaps tracking free/busy i-nodes and blocks.
4.  **Open File Table**: Volatile table tracking the current cursor position for active file descriptors.

---

## Concurrency & Synchronization

The implementation ensures **thread-safety** for multi-threaded applications. To maximize performance, the system employs **fine-grained locking**, ensuring that multiple threads can operate on different parts of the file system simultaneously without global bottlenecks.
