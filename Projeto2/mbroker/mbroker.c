#include "mbroker.h"
#include "boxes.h"
#include "logging.h"
#include "operations.h"
#include "producer-consumer.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct _req_ {
    uint8_t opcode;
    char cli_pname[PIPE_NAME_MAX_LEN + 1];
    union {
        char box_name[BOX_NAME_MAX_LEN + 1];
        struct {};
    };
} Request;

pthread_rwlock_t ending_lock = PTHREAD_RWLOCK_INITIALIZER;
struct _closing_vars_ {
    bool ending;
    size_t max_sessions;
    char *register_fifo_name;
    int register_fifo_fd_rd;
    int register_fifo_fd_wr;
    pthread_t *working_threads;
    pc_queue_t *queue;
} cv;

void init_server() { cv.ending = false; }

// Request for mbroker to register a publisher
void req_pub_reg(char *cli_pname, char *box_name) {
    int r_cli = open(cli_pname, O_RDONLY); // publisher pipe's file descriptor
    if (r_cli == -1) {
        ERROR("Can't open publisher pipe. Canceling request.");
        return;
    }
    STATUS("Started session of publisher (%s)", cli_pname);

    // we use a do {} while(0); so that we can break in the middle of the
    // pseudo-loop to in the end close the pipe name
    do {
        int bi = box_search(box_name);
        // if the box doesn't exists, we end the session
        if (bi == -1) {
            ERROR("Cant find box with name \"%s\"", box_name);
            break;
        }

        // if we can't register the publisher in the box, then we end the
        // session
        if (box_register_pub(bi) != 0) {
            ERROR("Cant register publisher");
            break;
        }

        while (1) {
            uint8_t opcode;
            ssize_t total_rd = read(r_cli, &opcode, OPCODE_LEN);
            if (total_rd == 0) {
                break;
            }

            // End session if opcode received is wrong
            if (opcode != PUB_SENDMSG_SV) {
                ERROR("Wrong opcode: %d | expected: %d", opcode,
                      PUB_SENDMSG_SV);
                break;
            }

            uint8_t msg[MESSAGE_MAX_LEN];
            total_rd = read(r_cli, msg, MESSAGE_MAX_LEN);
            // End session if there's bytes missing from reading fifo
            if (total_rd != MESSAGE_MAX_LEN) {
                ERROR("Couldn't read the exepcted number of bytes.");
                break;
            }

            STATUS("Message received from publisher");

            ssize_t total_wr = box_write(bi, msg, MESSAGE_MAX_LEN);
            if (total_wr == -1) {
                ERROR("error reading from box");
                break;
            }

            box_notify_new_write(bi);
        }

        box_unregister_pub(bi);
    } while (0);

    STATUS("Stopped session of publisher (%s)", cli_pname);
    close(r_cli);
}

// Request for mbroker to register a subscriber
void req_sub_reg(char *cli_pname, char *box_name) {
    int w_cli = open(cli_pname, O_WRONLY); // subscriber pipe's file descriptor
    if (w_cli == -1) {
        ERROR("Can't open sub pipe %s.", cli_pname);
        return;
    }
    STATUS("Started session of subscriber (%s)", cli_pname);
    signal(SIGPIPE, SIG_IGN);

    // we use a do {} while(0); so that we can break in the middle of the
    // pseudo-loop to in the end close the pipe name
    do {
        int bi = box_search(box_name);
        if (bi == -1) {
            ERROR("Can't find box with name \"%s\"", box_name);
            break;
        }

        int id = box_register_sub(bi);
        if (id < 0) {
            ERROR("Can't register subscriber");
            break;
        }

        size_t bsize = OPCODE_LEN + MESSAGE_MAX_LEN;
        uint8_t buffer[bsize], opcode = SV_SENDMSG_SUB;
        memcpy(buffer, &opcode, OPCODE_LEN);

        while (1) {
            memset(buffer + OPCODE_LEN, 0, bsize - OPCODE_LEN);

            box_wait_for_new_write(bi, id);
            ssize_t total_rd =
                box_read(bi, id, buffer + OPCODE_LEN, MESSAGE_MAX_LEN);

            if (total_rd == -1) {
                ERROR("error reading from box");
                break;
            }

            int j = OPCODE_LEN; // search for the end of the message
            for (; j < bsize && buffer[j] != 0; j++)
                ;

            // set all the remaining bytes of the buffer to 0
            for (; j < bsize; j++)
                buffer[j] = 0;

            ssize_t total_wr = write(w_cli, buffer, bsize);
            if (total_wr < bsize) {
                ERROR("Error writing into the sub pipe");
                break;
            }
            if (errno == EPIPE) {
                signal(SIGPIPE, SIG_DFL);
                errno = 0;
                STATUS("Subscriber disconnected from pipe");
                break;
            }

            STATUS("Message sent to subscriber");
        }

        box_unregister_sub(bi, id);

    } while (0);

    STATUS("Stopped session of subscriber (%s)", cli_pname);
    close(w_cli);
}

// Request for mbroker to create a box
void req_box_crt(char *cli_pname, char *box_name) {
    int w_cli = open(cli_pname, O_WRONLY);
    if (w_cli == -1) {
        ERROR("Can't open client pipe.");
        return;
    }
    STATUS("Started creating session of manager (%s)", cli_pname);

    size_t bsize = OPCODE_LEN + RET_CODE_LEN + ERR_MESSAGE_MAX_LEN;
    uint8_t buffer[bsize];

    // we use a do {} while(0); so that we can break in the middle of the
    // pseudo-loop to in the end close the pipe name
    do {
        uint8_t *buf_ptr, opcode = BOX_CRT_ANS;
        int32_t ok_code = OK_RET_CODE, err_code = ERR_RET_CODE;

        memset(buffer, 0, bsize);

        buf_ptr = buffer;

        wr_inc(buf_ptr, &opcode, OPCODE_LEN);

        if (box_name[0] == 0) {
            ERROR("Empty box names are not allowed.");

            wr_inc(buf_ptr, &err_code, RET_CODE_LEN);
            // just in case the error message is larger than MESSAGE_MAX_LEN
            wr_inc(buf_ptr, ERR_MSG_EMPTY_BOX_NAME,
                   min(strlen(ERR_MSG_EMPTY_BOX_NAME), ERR_MESSAGE_MAX_LEN));
            break;
        }

        int bi = box_search(box_name);
        if (bi != -1) { // if box already exists
            ERROR("box %s already exists", box_name);
            wr_inc(buf_ptr, &err_code, RET_CODE_LEN);

            // just in case the error message is larger than MESSAGE_MAX_LEN
            wr_inc(buf_ptr, ERR_MSG_BOX_EXISTS,
                   min(strlen(ERR_MSG_BOX_EXISTS), ERR_MESSAGE_MAX_LEN));
            break;
        }

        if (box_add(box_name) == -1) { // if not possible to create the box
            wr_inc(buf_ptr, &err_code, RET_CODE_LEN);
            // just in case the error message is larger than MESSAGE_MAX_LEN
            wr_inc(buf_ptr, ERR_MSG_CANT_CREATE,
                   min(strlen(ERR_MSG_CANT_CREATE), ERR_MESSAGE_MAX_LEN));
            ERROR("error adding %s box", box_name);
            break;
        }

        STATUS("Box %s added", box_name);

        wr_inc(buf_ptr, &ok_code, RET_CODE_LEN);
    } while (0);

    ssize_t total_wr = write(w_cli, buffer, bsize);
    if (total_wr == -1) {
        ERROR("Couldn't write in client pipe %s.", cli_pname);
    }

    STATUS("Stopped creating session of manager (%s)", cli_pname);
    close(w_cli);
}

// Request for mbroker to remove a box
void req_box_rem(char *cli_pname, char *box_name) {
    int w_cli = open(cli_pname, O_WRONLY);
    if (w_cli == -1) {
        ERROR("Can't open client pipe.");
        return;
    }
    STATUS("Started removing session of manager (%s)", cli_pname);

    size_t bsize = OPCODE_LEN + RET_CODE_LEN + ERR_MESSAGE_MAX_LEN;
    uint8_t buffer[bsize];

    // we use a do {} while(0); so that we can break in the middle of the
    // pseudo-loop to in the end close the pipe name
    do {
        uint8_t *buf_ptr, opcode = BOX_REM_ANS;
        int32_t ok_code = OK_RET_CODE, err_code = ERR_RET_CODE;

        buf_ptr = buffer;

        memset(buffer, 0, bsize);

        wr_inc(buf_ptr, &opcode, OPCODE_LEN);

        int bi = box_search(box_name);
        if (bi == -1) { // if box does not exists
            wr_inc(buf_ptr, &err_code, RET_CODE_LEN);

            // just in case the error message is larger than MESSAGE_MAX_LEN
            wr_inc(buf_ptr, ERR_MSG_BOX_NOT_FOUND,
                   min(strlen(ERR_MSG_BOX_NOT_FOUND), MESSAGE_MAX_LEN));
            break;
        }

        if (box_rem(bi) == -1) {
            wr_inc(buf_ptr, &err_code, RET_CODE_LEN);

            // just in case the error message is larger than MESSAGE_MAX_LEN
            wr_inc(buf_ptr, ERR_MSG_CANT_REMOVE,
                   min(strlen(ERR_MSG_CANT_REMOVE), MESSAGE_MAX_LEN));
            break;
        }

        STATUS("Box %s removed", cli_pname);

        wr_inc(buf_ptr, &ok_code, RET_CODE_LEN);
    } while (0);

    ssize_t total_wr = write(w_cli, buffer, bsize);
    if (total_wr == -1) {
        ERROR("Couldn't write in client pipe %s.", cli_pname);
    }

    STATUS("Stopped removing session of manager (%s)", cli_pname);
    close(w_cli);
}

// Request for mbroker to list all boxes
void req_box_lst(char *cli_pname) {
    int w_cli = open(cli_pname, O_WRONLY);
    if (w_cli == -1) {
        ERROR("Can't open client pipe.");
        return;
    }

    STATUS("Started listing session of manager (%s)", cli_pname);

    size_t bsize = OPCODE_LEN + LAST_BIT_LEN + BOX_NAME_MAX_LEN + BOX_SIZE_LEN +
                   NUM_PUB_LEN + NUM_SUB_LEN;
    uint8_t buffer[bsize];

    // we use a do {} while(0); so that we can break in the middle of the
    // pseudo-loop to in the end close the pipe name
    do {
        Binfo inf[BOX_MAX_NUM];

        int count = box_lst(inf, BOX_MAX_NUM);

        uint8_t opcode = BOX_LST_ANS, *buf_ptr, last;

        buf_ptr = buffer;
        wr_inc(buf_ptr, &opcode, OPCODE_LEN);

        if (count == 0) {
            memset(buf_ptr, 0, bsize - OPCODE_LEN);
            last = LAST_BIT;
            wr_inc(buf_ptr, &last, LAST_BIT_LEN);
            ssize_t total_wr = write(w_cli, buffer, bsize);
            if (total_wr == -1) {
                ERROR("Couldn't write in client pipe.");
                break;
            }
            STATUS("No boxes listed");
        } else {
            for (int i = 0; i < count; i++) {
                last = (i == count - 1) ? LAST_BIT : NOT_LAST_BIT;

                buf_ptr = buffer + OPCODE_LEN;

                wr_inc(buf_ptr, &last, LAST_BIT_LEN);

                wr_inc(buf_ptr, inf[i].name, BOX_NAME_MAX_LEN);

                wr_inc(buf_ptr, &inf[i].size, BOX_SIZE_LEN);

                wr_inc(buf_ptr, &inf[i].n_pubs, NUM_PUB_LEN);

                wr_inc(buf_ptr, &inf[i].n_subs, NUM_SUB_LEN);

                ssize_t total_wr = write(w_cli, buffer, bsize);
                if (total_wr == -1) {
                    ERROR("Couldn't write in client pipe.");
                    break;
                }
            }
            STATUS("All boxes listed");
        }
    } while (0);

    STATUS("Stopped listing session of manager (%s)", cli_pname);
    close(w_cli);
}

// Function that handles a request using its opcode
void request_handler(Request *req) {
    // We print to stderr the status of the program with the macro STATUS()
    // everytime we start and end a session and the errors to the stderr in each
    // specific request handler function
    switch (req->opcode) {
    case PUB_REG:
        req_pub_reg(req->cli_pname, req->box_name); // code 1
        break;
    case SUB_REG:
        req_sub_reg(req->cli_pname, req->box_name); // code 2
        break;
    case BOX_CRT:
        req_box_crt(req->cli_pname, req->box_name); // code 3
        break;
    case BOX_REM:
        req_box_rem(req->cli_pname, req->box_name); // code 5
        break;
    case BOX_LST:
        req_box_lst(req->cli_pname); // code 7
        break;
    default:
        ERROR("Invalid OPCODE. Canceling request.");
    }
}

// Function that each worker thread executes when is created, to handle the
// requests that the mbroker receives. Receives the queue of requests as a
// parameter. Frees the memory allocated to each request handled.
void *worker_thread_fn(void *arg) {
    pc_queue_t *queue = (pc_queue_t *)arg;

    // Infinite loop to handle the requests in the queue
    if (pthread_rwlock_rdlock(&ending_lock) != 0) {
        WARN("failed to lock rdlock: %s", strerror(errno));
    }
    while (!cv.ending) {
        if (pthread_rwlock_unlock(&ending_lock) != 0) {
            WARN("failed to unlock rdlock: %s", strerror(errno));
        }

        Request *req = (Request *)pcq_dequeue(queue);
        request_handler(req);
        free(req);

        if (pthread_rwlock_rdlock(&ending_lock) != 0) {
            WARN("failed to lock rdlock: %s", strerror(errno));
        }
    }
    if (pthread_rwlock_unlock(&ending_lock) != 0) {
        WARN("failed to unlock rdlock: %s", strerror(errno));
    }

    return NULL;
}

int close_server(int exit_code) {
    STATUS("Closing server");

    if (pthread_rwlock_wrlock(&ending_lock) != 0) {
        WARN("failed to lock wrlock: %s", strerror(errno));
    }
    cv.ending = true;
    if (pthread_rwlock_unlock(&ending_lock) != 0) {
        WARN("failed to unlock wrlock: %s", strerror(errno));
    }

    for (int i = 0; i < cv.max_sessions; i++) {
        pthread_join(cv.working_threads[i], NULL);
    }

    while (cv.queue->pcq_current_size > 0) {
        free((Request *)pcq_dequeue(cv.queue));
    }

    pcq_destroy(cv.queue);
    free(cv.queue);
    destroy_boxes();

    close(cv.register_fifo_fd_rd);
    close(cv.register_fifo_fd_wr);
    if (unlink(cv.register_fifo_name) != 0 && errno != ENOENT) {
        PANIC("Error unlinking register fifo before creation.");
    }

    exit(exit_code);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: mbroker <register_pipe_name> <max_sessions>\n");
        return -1;
    }

    if (strlen(argv[1]) > PIPE_NAME_MAX_LEN) {
        fprintf(stderr, "Pipe name size should be lesser than %d\n",
                PIPE_NAME_MAX_LEN);
        fprintf(stderr, "usage: mbroker <register_pipe_name> <max_sessions>\n");
        return -1;
    }
    char reg_fifo_name[PIPE_NAME_MAX_LEN + 1];
    reg_fifo_name[PIPE_NAME_MAX_LEN] = 0;
    strncpy(reg_fifo_name, argv[1], PIPE_NAME_MAX_LEN);

    cv.register_fifo_name = reg_fifo_name;

    int int_rd = (size_t)atoi(argv[2]);
    if (int_rd <= 0) {
        fprintf(stderr, "Max sessions have to be a number greater than 1\n");
        fprintf(stderr, "usage: mbroker <register_pipe_name> <max_sessions>\n");
        return -1;
    }
    size_t max_sessions = cv.max_sessions = (size_t)int_rd;

    if (unlink(reg_fifo_name) != 0 && errno != ENOENT) {
        fprintf(stderr, "Error unlinking register fifo before creation.\n");
    }

    if (mkfifo(reg_fifo_name, PIPE_PERMS) != 0) {
        PANIC("Can't create fifo");
    }

    int r_svfifo = open(reg_fifo_name, O_RDONLY | O_NONBLOCK);

    if (r_svfifo == -1) {
        PANIC("Error opening register fifo (rd mode).");
        if (unlink(reg_fifo_name) != 0 && errno != ENOENT) {
            PANIC("Error unlinking register fifo before creation.");
        }
        return -1;
    }

    cv.register_fifo_fd_rd = r_svfifo;

    fcntl(r_svfifo, F_SETFL, fcntl(r_svfifo, F_GETFL, 0) & ~O_NONBLOCK);

    int wr_end_fifo = open(reg_fifo_name, O_WRONLY);
    if (wr_end_fifo == -1) {
        PANIC("Error opening register fifo (wr mode).");
        close(r_svfifo);
        if (unlink(reg_fifo_name) != 0 && errno != ENOENT) {
            PANIC("Error unlinking register fifo before creation.");
        }
        return -1;
    }

    cv.register_fifo_fd_wr = wr_end_fifo;

    if (init_boxes(BOX_MAX_NUM, MSG_MAX_NUM_BOX * MESSAGE_MAX_LEN,
                   max_sessions) == -1) {
        close(r_svfifo);
        close(wr_end_fifo);
        if (unlink(reg_fifo_name) != 0 && errno != ENOENT) {
            PANIC("Error unlinking register fifo before creation.");
        }
        return -1;
    }

    pc_queue_t queue;
    cv.queue = &queue;
    if (pcq_create(&queue, max_sessions) == -1) {
        destroy_boxes();
        close(r_svfifo);
        close(wr_end_fifo);
        if (unlink(reg_fifo_name) != 0 && errno != ENOENT) {
            PANIC("Error unlinking register fifo before creation.");
        }
        return -1;
    }

    pthread_t wt[max_sessions];
    cv.working_threads = wt;
    for (int i = 0; i < max_sessions; i++) {
        if (pthread_create(&wt[i], NULL, worker_thread_fn, &queue) != 0) {
            WARN("failed to create thread: %s", strerror(errno));
        }
    }

    while (1) {
        Request *req = (Request *)malloc(sizeof(Request));

        STATUS("Waiting for requests");
        ssize_t total_rd = read(r_svfifo, &req->opcode, OPCODE_LEN);
        if (total_rd == 0) {
            break;
        }
        if (total_rd == -1) {
            break;
        }

        if (req->opcode < FIRST_OPCODE || req->opcode > LAST_OPCODE) {
            ERROR("Wrong OPCODE : %d", req->opcode);
            break;
        }

        STATUS("Received a request");

        memset(req->cli_pname, 0, PIPE_NAME_MAX_LEN + 1);
        total_rd = read(r_svfifo, req->cli_pname, PIPE_NAME_MAX_LEN);
        if (total_rd != PIPE_NAME_MAX_LEN) {
            ERROR("Error reading client pipe name (num of bytes shorter than "
                  "expected).");
            break;
        }

        if (req->opcode != BOX_LST) {
            memset(req->box_name, 0, BOX_NAME_MAX_LEN + 1);
            total_rd = read(r_svfifo, req->box_name, BOX_NAME_MAX_LEN);
            if (total_rd != BOX_NAME_MAX_LEN) {
                ERROR("Error reading box name (num of bytes shorter than "
                      "expected).");
                break;
            }
        }

        pcq_enqueue(&queue, req);
    }

    close_server(0);
}