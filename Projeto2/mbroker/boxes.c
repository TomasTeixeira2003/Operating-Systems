#include "boxes.h"
#include "logging.h"
#include "operations.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define NO_FD (-1)

struct _bspecs_ {
    char name[BOX_NAME_MAX_LEN + 1];
    bool has_pub;
    bool exists;
    size_t n_subs;
    int pub_fd;
    size_t pub_n_writes;
    int *subs_fd;
    size_t *subs_n_reads;
} boxes[BOX_MAX_NUM];

size_t max_open;

static pthread_rwlock_t box_locks[BOX_MAX_NUM];
// specific new_msg_lock mutex because pthread_cond_wait() cant take a rwlock
pthread_mutex_t new_msg_lock[BOX_MAX_NUM];
pthread_cond_t new_msg[BOX_MAX_NUM];

// Opens the box with the index box_index in the mode passed as argument
int box_open(char *box_name, tfs_file_mode_t mode) {
    char path[strlen(box_name) + 2]; // because of '/' and '\0'
    strcpy(path, "/");
    strcat(path, box_name);

    return tfs_open(path, mode);
}

// Creates the box with the name box_name in the boxes array
int box_add(char *box_name) {
    for (int i = 0; i < BOX_MAX_NUM; i++) {
        if (pthread_rwlock_wrlock(&box_locks[i]) != 0) {
            WARN("failed to lock wrlock: %s", strerror(errno));
        }
        if (!boxes[i].exists) {
            int fd = box_open(box_name, TFS_O_CREAT);
            if (fd == -1) {
                ERROR("creating %s box in tfs - error", box_name);
                if (pthread_rwlock_unlock(&box_locks[i]) != 0) {
                    WARN("failed to unlock wrlock: %s", strerror(errno));
                }
                return -1;
            }
            tfs_close(fd);

            // Initializing the box
            strncpy(boxes[i].name, box_name, BOX_NAME_MAX_LEN);
            boxes[i].exists = true;
            boxes[i].has_pub = false;
            boxes[i].n_subs = 0;
            boxes[i].pub_n_writes = 0;

            boxes[i].pub_fd = NO_FD;
            for (int j = 0; j < max_open; j++) {
                boxes[i].subs_fd[j] = NO_FD;
                boxes[i].subs_n_reads[j] = 0;
            }

            if (pthread_rwlock_unlock(&box_locks[i]) != 0) {
                WARN("failed to unlock wrlock: %s", strerror(errno));
            }
            return 0;
        }
        if (pthread_rwlock_unlock(&box_locks[i]) != 0) {
            WARN("failed to unlock wrlock: %s", strerror(errno));
        }
    }
    ERROR("no space for boxes");
    return -1;
}

// Remove the box at the index box_index in the boxes array
int box_rem(int box_index) {
    int bi = box_index;

    if (pthread_rwlock_wrlock(&box_locks[bi]) != 0) {
        WARN("failed to lock wrlock: %s", strerror(errno));
    }
    char path[strlen(boxes[bi].name) + 2]; // because of '/' and '\0'
    strcpy(path, "/");
    strcat(path, boxes[bi].name);

    if (tfs_unlink(path) == -1) {
        if (pthread_rwlock_unlock(&box_locks[bi]) != 0) {
            WARN("failed to unlock wrlock: %s", strerror(errno));
        }
        ERROR("box not in tfs");
        return -1;
    }

    // Reseting the box
    memset(boxes[bi].name, 0, BOX_NAME_MAX_LEN + 1);
    boxes[bi].exists = false;
    boxes[bi].n_subs = 0;
    boxes[bi].has_pub = false;
    boxes[bi].pub_n_writes = 0;

    if (boxes[bi].pub_fd != NO_FD) {
        tfs_close(boxes[bi].pub_fd);
    }

    boxes[bi].pub_fd = NO_FD;
    for (int j = 0; j < max_open; j++) {
        if (boxes[bi].subs_fd[j] != NO_FD) {
            tfs_close(boxes[bi].subs_fd[j]);
        }
        boxes[bi].subs_fd[j] = NO_FD;
        boxes[bi].subs_n_reads[j] = 0;
    }

    if (pthread_rwlock_unlock(&box_locks[bi]) != 0) {
        WARN("failed to unlock wrlock: %s", strerror(errno));
    }
    return 0;
}

// FIXME comentar
int box_lst(Binfo *arr, size_t size) {
    int count = 0;

    arr[count].n_pubs = NO_PUB;
    arr[count].n_subs = 0;
    memset(arr[count].name, 0, BOX_NAME_MAX_LEN + 1);
    arr[count].size = 0;

    for (int i = 0; i < size; i++) {
        if (pthread_rwlock_rdlock(&box_locks[i]) != 0) {
            WARN("failed to lock rdlock: %s", strerror(errno));
        }
        if (boxes[i].exists) {
            Binfo *bi = arr + count;
            bi->n_pubs = (boxes[i].has_pub) ? HAS_PUB : NO_PUB;
            bi->n_subs = boxes[i].n_subs;
            strcpy(bi->name, boxes[i].name);
            bi->size = boxes[i].pub_n_writes * MESSAGE_MAX_LEN;

            count++;
        }
        if (pthread_rwlock_unlock(&box_locks[i]) != 0) {
            WARN("failed to unlock rdlock: %s", strerror(errno));
        }
    }

    return count;
}

// Returns the position of the box in the array
int box_search(char *box_name) {
    for (int i = 0; i < BOX_MAX_NUM; i++) {
        if (pthread_rwlock_rdlock(&box_locks[i]) != 0) {
            WARN("failed to lock rdlock: %s", strerror(errno));
        }
        if (boxes[i].exists && !strcmp(box_name, boxes[i].name)) {
            if (pthread_rwlock_unlock(&box_locks[i]) != 0) {
                WARN("failed to unlock rdlock: %s", strerror(errno));
            }
            return i;
        }
        if (pthread_rwlock_unlock(&box_locks[i]) != 0) {
            WARN("failed to unlock rdlock: %s", strerror(errno));
        }
    }

    return -1;
}

// Write size bytes from the buffer to the box with index box_index
ssize_t box_write(int box_index, uint8_t *buffer, size_t size) {
    int bi = box_index;
    if (pthread_rwlock_rdlock(&box_locks[bi]) != 0) {
        WARN("failed to lock rdlock: %s", strerror(errno));
    }

    if (!boxes[bi].exists) {
        if (pthread_rwlock_unlock(&box_locks[bi]) != 0) {
            WARN("failed to unlock rdlock: %s", strerror(errno));
        }
        ERROR("The box does not exist");
        return -1;
    }

    ssize_t total_wr = tfs_write(boxes[bi].pub_fd, buffer, size);

    if (total_wr == -1) {
        ERROR("Error writing to the box");
        if (pthread_rwlock_unlock(&box_locks[bi]) != 0) {
            WARN("failed to unlock rdlock: %s", strerror(errno));
        }
        return -1;
    } else if (total_wr < size) {
        ERROR("Didn't write all the bytes");
    }

    if (pthread_rwlock_unlock(&box_locks[bi]) != 0) {
        WARN("failed to unlock rdlock: %s", strerror(errno));
    }
    return total_wr;
}

// FIXME comentar
ssize_t box_read(int box_index, int sub_id, uint8_t *buffer, size_t size) {
    int bi = box_index;
    if (pthread_rwlock_rdlock(&box_locks[bi]) != 0) {
        WARN("failed to lock rdlock: %s", strerror(errno));
    }

    if (!boxes[bi].exists) {
        if (pthread_rwlock_unlock(&box_locks[bi]) != 0) {
            WARN("failed to unlock rdlock: %s", strerror(errno));
        }
        return -1;
    }

    ssize_t total_rd = tfs_read(boxes[bi].subs_fd[sub_id], buffer, size);

    if (total_rd == -1) {
        ERROR("Error reading from the box");
        if (pthread_rwlock_unlock(&box_locks[bi]) != 0) {
            WARN("failed to unlock rdlock: %s", strerror(errno));
        }
        return -1;
    } else if (total_rd < size) {
        ERROR("Didn't read all the bytes");
    }

    if (pthread_rwlock_unlock(&box_locks[bi]) != 0) {
        WARN("failed to unlock rdlock: %s", strerror(errno));
    }
    return total_rd;
}

// Register a publisher to a box
int box_register_pub(int box_index) {
    int bi = box_index;

    if (pthread_rwlock_wrlock(&box_locks[bi]) != 0) {
        WARN("failed to lock wrlock: %s", strerror(errno));
    }

    if (!boxes[bi].exists) {
        ERROR("Box doesn't exist");
        if (pthread_rwlock_unlock(&box_locks[bi]) != 0) {
            WARN("failed to unlock wrlock: %s", strerror(errno));
        }
        return -1;
    }

    if (boxes[bi].has_pub) {
        ERROR("Box already has a pub");
        if (pthread_rwlock_unlock(&box_locks[bi]) != 0) {
            WARN("failed to unlock wrlock: %s", strerror(errno));
        }
        return -1;
    }
    boxes[bi].has_pub = true;

    boxes[bi].pub_fd = box_open(boxes[bi].name, TFS_O_APPEND);

    if (pthread_rwlock_unlock(&box_locks[bi]) != 0) {
        WARN("failed to unlock wrlock: %s", strerror(errno));
    }

    return 0;
}

// Unregister a publisher from a box
int box_unregister_pub(int box_index) {
    int bi = box_index;

    if (pthread_rwlock_wrlock(&box_locks[bi]) != 0) {
        WARN("failed to lock wrlock: %s", strerror(errno));
    }

    if (!boxes[bi].exists) {
        if (pthread_rwlock_unlock(&box_locks[bi]) != 0) {
            WARN("failed to unlock wrlock: %s", strerror(errno));
        }
        ERROR("Box doesn't exist");
        return -1;
    }

    if (!boxes[bi].has_pub) {
        if (pthread_rwlock_unlock(&box_locks[bi]) != 0) {
            WARN("failed to unlock wrlock: %s", strerror(errno));
        }
        ERROR("Box doesn't have a pub");
        return -1;
    }
    boxes[bi].has_pub = false;

    tfs_close(boxes[bi].pub_fd);

    boxes[bi].pub_fd = NO_FD;

    if (pthread_rwlock_unlock(&box_locks[bi]) != 0) {
        WARN("failed to unlock wrlock: %s", strerror(errno));
    }

    return 0;
}

// Register a subscriber to a box
int box_register_sub(int box_index) {
    int bi = box_index;

    if (pthread_rwlock_wrlock(&box_locks[bi]) != 0) {
        WARN("failed to lock wrlock: %s", strerror(errno));
    }

    if (!boxes[bi].exists) {
        if (pthread_rwlock_unlock(&box_locks[bi]) != 0) {
            WARN("failed to unlock wrlock: %s", strerror(errno));
        }
        ERROR("Box doesn't exist");
        return -1;
    }

    for (int i = 0; i < max_open; i++) {
        if (boxes[bi].subs_fd[i] == NO_FD) {
            int fd = box_open(boxes[bi].name, 0);
            if (fd == -1) {
                ERROR("Cant open box : %s", boxes[bi].name);
                if (pthread_rwlock_unlock(&box_locks[bi]) != 0) {
                    WARN("failed to unlock wrlock: %s", strerror(errno));
                }
                return -1;
            }
            boxes[bi].subs_fd[i] = fd;
            boxes[bi].n_subs++;
            if (pthread_rwlock_unlock(&box_locks[bi]) != 0) {
                WARN("failed to unlock wrlock: %s", strerror(errno));
            }
            return i;
        }
    }

    if (pthread_rwlock_unlock(&box_locks[bi]) != 0) {
        WARN("failed to unlock wrlock: %s", strerror(errno));
    }

    return -1;
}

// Unregister a subscriber from a box
int box_unregister_sub(int box_index, int sub_id) {
    int bi = box_index;

    if (pthread_rwlock_wrlock(&box_locks[bi]) != 0) {
        WARN("failed to lock wrlock: %s", strerror(errno));
    }

    if (!boxes[bi].exists) {
        if (pthread_rwlock_unlock(&box_locks[bi]) != 0) {
            WARN("failed to unlock wrlock: %s", strerror(errno));
        }
        ERROR("Box doesn't exist");
        return -1;
    }

    if (boxes[bi].subs_fd[sub_id] == NO_FD) {
        if (pthread_rwlock_unlock(&box_locks[bi]) != 0) {
            WARN("failed to unlock wrlock: %s", strerror(errno));
        }
        ERROR("No file descriptor associated");
        return -1;
    }

    tfs_close(boxes[bi].subs_fd[sub_id]);
    boxes[bi].subs_fd[sub_id] = NO_FD;
    boxes[bi].subs_n_reads[sub_id] = 0;
    boxes[bi].n_subs--;

    if (pthread_rwlock_unlock(&box_locks[bi]) != 0) {
        WARN("failed to unlock wrlock: %s", strerror(errno));
    }

    return 0;
}

// Notify a box's subscribers that there is a new message
void box_notify_new_write(int box_index) {
    int bi = box_index;

    if (pthread_mutex_lock(&new_msg_lock[bi]) != 0) {
        WARN("failed to lock mutex: %s", strerror(errno));
    }
    if (pthread_rwlock_wrlock(&box_locks[bi]) != 0) {
        WARN("failed to lock wrlock: %s", strerror(errno));
    }

    boxes[bi].pub_n_writes++;
    if (pthread_cond_broadcast(&new_msg[bi]) != 0) {
        WARN("failed to broadcast conditional variable: %s", strerror(errno));
    }

    if (pthread_rwlock_unlock(&box_locks[bi]) != 0) {
        WARN("failed to unlock wrlock: %s", strerror(errno));
    }
    if (pthread_mutex_unlock(&new_msg_lock[bi]) != 0) {
        WARN("failed to unlock mutex: %s", strerror(errno));
    }

    return;
}

// FIXME comentar
void box_wait_for_new_write(int box_index, int sub_id) {
    int bi = box_index;

    if (pthread_mutex_lock(&new_msg_lock[bi]) != 0) {
        WARN("failed to lock mutex: %s", strerror(errno));
    }
    if (pthread_rwlock_wrlock(&box_locks[bi]) != 0) {
        WARN("failed to lock wrlock: %s", strerror(errno));
    }

    while (boxes[bi].pub_n_writes == boxes[bi].subs_n_reads[sub_id] ||
           !boxes[bi].exists) {
        if (pthread_rwlock_unlock(&box_locks[bi]) != 0) {
            WARN("failed to unlock wrlock: %s", strerror(errno));
        }
        if (pthread_cond_wait(&new_msg[bi], &new_msg_lock[bi]) != 0) {
            WARN("failed to wait for conditional variable: %s",
                 strerror(errno));
        }
        if (pthread_rwlock_wrlock(&box_locks[bi]) != 0) {
            WARN("failed to unlock wrlock: %s", strerror(errno));
        }
    }
    boxes[bi].subs_n_reads[sub_id]++;

    if (pthread_rwlock_unlock(&box_locks[bi]) != 0) {
        WARN("failed to unlock wrlock: %s", strerror(errno));
    }
    if (pthread_mutex_unlock(&new_msg_lock[bi]) != 0) {
        WARN("failed to unlock mutex: %s", strerror(errno));
    }
}

// Initializes the array of boxes with non-existing boxes
int init_boxes(size_t num_boxes, size_t box_capacity, size_t max_subs) {
    max_open = max_subs;

    tfs_params params = tfs_default_params();
    params.max_inode_count = params.max_block_count = num_boxes;
    params.block_size = box_capacity;
    params.max_open_files_count = max_open;
    tfs_init(&params);

    for (int i = 0; i < BOX_MAX_NUM; i++) {
        memset(boxes[i].name, 0, BOX_NAME_MAX_LEN + 1);
        boxes[i].has_pub = false;
        boxes[i].exists = false;
        boxes[i].n_subs = 0;
        boxes[i].pub_n_writes = 0;
        boxes[i].subs_fd = (int *)malloc(sizeof(int) * max_open);
        if (boxes[i].subs_fd == NULL) {
            return -1;
        }
        boxes[i].subs_n_reads = (size_t *)malloc(sizeof(size_t) * max_open);
        if (boxes[i].subs_n_reads == NULL) {
            return -1;
        }
        boxes[i].pub_fd = NO_FD;
        for (int j = 0; j < max_open; j++) {
            boxes[i].subs_n_reads[j] = 0;
            boxes[i].subs_fd[j] = NO_FD;
        }

        if (pthread_rwlock_init(&box_locks[i], NULL) != 0) {
            WARN("failed to initilize rwlock: %s", strerror(errno));
        }
        if (pthread_mutex_init(&new_msg_lock[i], NULL) != 0) {
            WARN("failed to initialize mutex: %s", strerror(errno));
        }
        if (pthread_cond_init(&new_msg[i], NULL) != 0) {
            WARN("failed to initialize conditional variable: %s",
                 strerror(errno));
        }
    }

    return 0;
}

// Destroys all the boxes
void destroy_boxes() {
    tfs_destroy();

    for (int i = 0; i < BOX_MAX_NUM; i++) {
        free(boxes[i].subs_fd);
        free(boxes[i].subs_n_reads);
        if (pthread_rwlock_destroy(&box_locks[i]) != 0) {
            WARN("failed to destroy rwlock: %s", strerror(errno));
        }
        if (pthread_mutex_destroy(&new_msg_lock[i]) != 0) {
            WARN("failed to destroy mutex: %s", strerror(errno));
        }
        if (pthread_cond_destroy(&new_msg[i]) != 0) {
            WARN("failed to destroy conditional variable: %s", strerror(errno));
        }
    }
}