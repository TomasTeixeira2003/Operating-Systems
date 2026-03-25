#include "../mbroker/mbroker.h"
#include "logging.h"

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// struct that holds the sub fifo file descriptor to use it in signal handling
struct _sigstruct_ {
    int fifo_d;
} scont;

static void sig_handler(int sig) {
    if (sig == SIGINT) {
        // Set the signal handler back to sig_handler function after each trap.
        if (signal(SIGINT, sig_handler) == SIG_ERR) {
            exit(EXIT_FAILURE);
        }

        close(scont.fifo_d);

        // INFO("Caught SIGINT");
        return; // Resume execution at point of interruption
    }
}

int main(int argc, char **argv) {
    // In the case the number of arguments given while executing the sub command
    // are wrong, the execution is ended and is printed a message with the usage
    // of sub command to stderr
    if (argc != 4) {
        fprintf(stderr,
                "usage: sub <register_pipe_name> <pipe_name> <box_name>\n");
        return -1;
    }

    // In the case the length of the given register pipe name is greater than
    // PIPE_NAME_MAX_LEN the execution is ended and is printed a message with
    // the usage of sub command to stderr
    if (strlen(argv[1]) > PIPE_NAME_MAX_LEN) {
        fprintf(stderr, "Pipe name size should be lesser than %d\n",
                PIPE_NAME_MAX_LEN);
        fprintf(stderr,
                "usage: sub <register_pipe_name> <pipe_name> <box_name>\n");
        return -1;
    }
    char reg_fifo_name[PIPE_NAME_MAX_LEN + 1];
    memset(reg_fifo_name, 0, PIPE_NAME_MAX_LEN + 1);
    strncpy(reg_fifo_name, argv[1], PIPE_NAME_MAX_LEN);

    char *sufix_str;
    get_fifo_sufix(sufix_str);
    if (strlen(argv[2]) > PIPE_NAME_MAX_LEN - strlen(sufix_str)) {
        fprintf(stderr, "Pipe name size should be lesser than %ld (it depends \
                        the length of pid)\n",
                PIPE_NAME_MAX_LEN - strlen(sufix_str));
        fprintf(stderr,
                "usage: sub <register_pipe_name> <pipe_name> <box_name>\n");
        return -1;
    }
    char own_fifo_name[PIPE_NAME_MAX_LEN + 1];
    memset(own_fifo_name, 0, PIPE_NAME_MAX_LEN + 1);
    strcpy(own_fifo_name, argv[2]);
    strcat(own_fifo_name, sufix_str);
    free(sufix_str);

    if (strlen(argv[3]) > BOX_NAME_MAX_LEN) {
        fprintf(stderr, "Box name size should be lesser than %d\n",
                BOX_NAME_MAX_LEN);
        fprintf(stderr,
                "usage: sub <register_pipe_name> <pipe_name> <box_name>\n");
        return -1;
    }
    char box_name[BOX_NAME_MAX_LEN + 1];
    memset(box_name, 0, BOX_NAME_MAX_LEN + 1);
    strncpy(box_name, argv[3], BOX_NAME_MAX_LEN);

    // Abrir a pipe do servidor para escrita
    int w_svfifo = open(reg_fifo_name, O_WRONLY);
    if (w_svfifo == -1) {
        fprintf(stderr, "Error opening server fifo.\n");
        return -1;
    }

    size_t to_send = OPCODE_LEN + PIPE_NAME_MAX_LEN + BOX_NAME_MAX_LEN;
    uint8_t sv_request[to_send], *sv_req_ptr, opcode = SUB_REG;

    sv_req_ptr = sv_request;

    wr_inc(sv_req_ptr, &opcode, OPCODE_LEN);
    wr_inc(sv_req_ptr, own_fifo_name, PIPE_NAME_MAX_LEN);
    wr_inc(sv_req_ptr, box_name, BOX_NAME_MAX_LEN);

    // Checks if the pine with the pipe name associated already exists
    if (access(own_fifo_name, F_OK) == 0) {
        fprintf(stderr, "The pipe given already exists.\n");
        return -1;
    }

    if (mkfifo(own_fifo_name, PIPE_PERMS) != 0) {
        PANIC("Can't create fifo");
    }

    ssize_t total_wr = write(w_svfifo, sv_request, to_send);
    if (total_wr == -1) {
        pre_quit(own_fifo_name);
        PANIC("Error writing to server fifo.");
    }
    STATUS("Server request sent");

    close(w_svfifo);

    // blocks here until mbroker opens the pipe in read mode
    int r_fifo = open(own_fifo_name, O_RDONLY);
    if (r_fifo == -1) {
        pre_quit(own_fifo_name);
        PANIC("Error opening own fifo.");
    }
    scont.fifo_d = r_fifo;

    if (signal(SIGINT, sig_handler) == SIG_ERR) {
        exit(EXIT_FAILURE);
    }

    size_t msg_count = 0;
    while (1) {
        ssize_t total_rd = read(r_fifo, &opcode, OPCODE_LEN);
        if (total_rd == 0) {
            STATUS("Closing (EOF reached)");
            break;
        }

        if (opcode != SV_SENDMSG_SUB) {
            ERROR("Error in opcode - %d vs %d", opcode, SV_SENDMSG_SUB);
            break;
        }

        // 2 carateres extra: 1 para o \n e outro para o \0
        char msg[MESSAGE_MAX_LEN + 1];
        msg[MESSAGE_MAX_LEN] = 0;
        total_rd = read(r_fifo, &msg, MESSAGE_MAX_LEN);
        if (total_rd != MESSAGE_MAX_LEN) {
            ERROR("EOF in the pipe (reading message)");
            break;
        }

        fprintf(stdout, "%s\n", msg);
        msg_count++;
    }

    close(r_fifo);

    fprintf(stdout, "Número de mensagens lidas: %ld\n", msg_count);

    pre_quit(own_fifo_name);

    return 0;
}
