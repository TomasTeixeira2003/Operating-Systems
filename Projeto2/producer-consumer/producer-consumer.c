#include "producer-consumer.h"
#include "../mbroker/mbroker.h"
#include "logging.h"

#include <errno.h>
#include <pthread.h>

// pcq_create: create a queue, with a given (fixed) capacity
//
// Memory: the queue pointer must be previously allocated
// (either on the stack or the heap)
int pcq_create(pc_queue_t *queue, size_t capacity) {
    queue->pcq_buffer = (void **)malloc(capacity * sizeof(void *));
    if (queue->pcq_buffer == NULL) {
        return -1;
    }

    queue->pcq_capacity = capacity;

    if (pthread_mutex_init(&queue->pcq_current_size_lock, NULL) != 0) {
        WARN("failed to create mutex: %s", strerror(errno));
    }
    queue->pcq_current_size = 0;

    if (pthread_mutex_init(&queue->pcq_head_lock, NULL) != 0) {
        WARN("failed to create mutex: %s", strerror(errno));
    }
    queue->pcq_head = 0;

    if (pthread_mutex_init(&queue->pcq_tail_lock, NULL) != 0) {
        WARN("failed to create mutex: %s", strerror(errno));
    }
    queue->pcq_tail = 0;

    if (pthread_mutex_init(&queue->pcq_pusher_condvar_lock, NULL) != 0) {
        WARN("failed to create mutex: %s", strerror(errno));
    }
    if (pthread_cond_init(&queue->pcq_pusher_condvar, NULL) != 0) {
        WARN("failed to create conditional variable: %s", strerror(errno));
    }

    if (pthread_mutex_init(&queue->pcq_popper_condvar_lock, NULL) != 0) {
        WARN("failed to create mutex: %s", strerror(errno));
    }
    if (pthread_cond_init(&queue->pcq_popper_condvar, NULL) != 0) {
        WARN("failed to create conditional variables: %s", strerror(errno));
    }

    return 0;
}

// pcq_destroy: releases the internal resources of the queue
//
// Memory: does not free the queue pointer itself
int pcq_destroy(pc_queue_t *queue) {
    if (pthread_mutex_destroy(&queue->pcq_current_size_lock) != 0) {
        WARN("failed to destroy mutex: %s", strerror(errno));
    }
    if (pthread_mutex_destroy(&queue->pcq_head_lock) != 0) {
        WARN("failed to destroy mutex: %s", strerror(errno));
    }
    if (pthread_mutex_destroy(&queue->pcq_tail_lock) != 0) {
        WARN("failed to destroy mutex: %s", strerror(errno));
    }
    if (pthread_mutex_destroy(&queue->pcq_pusher_condvar_lock) != 0) {
        WARN("failed to destroy mutex: %s", strerror(errno));
    }
    if (pthread_cond_destroy(&queue->pcq_pusher_condvar) != 0) {
        WARN("failed to destroy conditional variable: %s", strerror(errno));
    }
    if (pthread_mutex_destroy(&queue->pcq_popper_condvar_lock) != 0) {
        WARN("failed to destroy mutex: %s", strerror(errno));
    }
    if (pthread_cond_destroy(&queue->pcq_popper_condvar) != 0) {
        WARN("failed to destroy conditional variable: %s", strerror(errno));
    }
    free(queue->pcq_buffer);

    return 0;
}

// pcq_enqueue: insert a new element at the front of the queue
//
// If the queue is full, sleep until the queue has space
int pcq_enqueue(pc_queue_t *queue, void *elem) {
    if (pthread_mutex_lock(&queue->pcq_current_size_lock) != 0) {
        WARN("failed to lock mutex: %s", strerror(errno));
    }
    while (queue->pcq_current_size == queue->pcq_capacity) {
        if (pthread_cond_wait(&queue->pcq_pusher_condvar,
                              &queue->pcq_current_size_lock) != 0) {
            WARN("failed to wait for conditional variable: %s",
                 strerror(errno));
        }
    }
    if (pthread_mutex_unlock(&queue->pcq_current_size_lock) != 0) {
        WARN("failed to unlock mutex: %s", strerror(errno));
    }

    if (pthread_mutex_lock(&queue->pcq_tail_lock) != 0) {
        WARN("failed to lock mutex: %s", strerror(errno));
    }
    queue->pcq_buffer[queue->pcq_tail] = elem;
    queue->pcq_tail = (queue->pcq_tail + 1) % queue->pcq_capacity;
    if (pthread_mutex_unlock(&queue->pcq_tail_lock) != 0) {
        WARN("failed to unlock mutex: %s", strerror(errno));
    }

    if (pthread_mutex_lock(&queue->pcq_current_size_lock) != 0) {
        WARN("failed to lock mutex: %s", strerror(errno));
    }
    queue->pcq_current_size++;
    if (pthread_cond_signal(&queue->pcq_popper_condvar) != 0) {
        WARN("failed to siganl conditional variable: %s", strerror(errno));
    }
    if (pthread_mutex_unlock(&queue->pcq_current_size_lock) != 0) {
        WARN("failed to unlock mutex: %s", strerror(errno));
    }

    return 0;
}

// pcq_dequeue: remove an element from the back of the queue
//
// If the queue is empty, sleep until the queue has an element
void *pcq_dequeue(pc_queue_t *queue) {
    if (pthread_mutex_lock(&queue->pcq_current_size_lock) != 0) {
        WARN("failed to lock mutex: %s", strerror(errno));
    }
    while (queue->pcq_current_size == 0) {
        if (pthread_cond_wait(&queue->pcq_popper_condvar,
                              &queue->pcq_current_size_lock) != 0) {
            WARN("failed to wait for conditional variable: %s",
                 strerror(errno));
        }
    }
    if (pthread_mutex_unlock(&queue->pcq_current_size_lock) != 0) {
        WARN("failed to unlock mutex: %s", strerror(errno));
    }

    if (pthread_mutex_lock(&queue->pcq_head_lock) != 0) {
        WARN("failed to lock mutex: %s", strerror(errno));
    }
    void *elem = queue->pcq_buffer[queue->pcq_head];
    queue->pcq_head = (queue->pcq_head + 1) % queue->pcq_capacity;
    if (pthread_mutex_unlock(&queue->pcq_head_lock) != 0) {
        WARN("failed to unlock mutex: %s", strerror(errno));
    }

    if (pthread_mutex_lock(&queue->pcq_current_size_lock) != 0) {
        WARN("failed to lock mutex: %s", strerror(errno));
    }
    queue->pcq_current_size--;
    if (pthread_cond_signal(&queue->pcq_pusher_condvar) != 0) {
        WARN("failed to signal conditional variable: %s", strerror(errno));
    }
    if (pthread_mutex_unlock(&queue->pcq_current_size_lock) != 0) {
        WARN("failed to unlock mutex: %s", strerror(errno));
    }

    return elem;
}