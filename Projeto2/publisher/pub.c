#include "../mbroker/mbroker.h"
#include "logging.h"

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

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr,
                "usage: pub <register_pipe_name> <pipe_name> <box_name>\n");
        return -1;
    }

    if (strlen(argv[1]) > PIPE_NAME_MAX_LEN) {
        fprintf(stderr, "Pipe name size should be lesser than %d\n",
                PIPE_NAME_MAX_LEN);
        fprintf(stderr,
                "usage: pub <register_pipe_name> <pipe_name> <box_name>\n");
        return -1;
    }
    char reg_fifo_name[PIPE_NAME_MAX_LEN + 1];
    memset(reg_fifo_name, 0, PIPE_NAME_MAX_LEN + 1);
    strncpy(reg_fifo_name, argv[1], PIPE_NAME_MAX_LEN);

    char *sufix_str;
    get_fifo_sufix(sufix_str);
    if (strlen(argv[2]) > PIPE_NAME_MAX_LEN - strlen(sufix_str)) {
        fprintf(stderr, "Pipe name size should be lesser than %ld (it depends \
                        on the length of pid)\n",
                PIPE_NAME_MAX_LEN - strlen(sufix_str));
        fprintf(stderr,
                "usage: pub <register_pipe_name> <pipe_name> <box_name>\n");
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
                "usage: pub <register_pipe_name> <pipe_name> <box_name>\n");
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
    uint8_t sv_request[to_send], *sv_req_ptr, opcode = PUB_REG;

    sv_req_ptr = sv_request;

    wr_inc(sv_req_ptr, &opcode, OPCODE_LEN);
    wr_inc(sv_req_ptr, own_fifo_name, PIPE_NAME_MAX_LEN);
    wr_inc(sv_req_ptr, box_name, BOX_NAME_MAX_LEN);

    // Checks if the pipe with the pipe name associated already exists
    if (access(own_fifo_name, F_OK) == 0) {
        fprintf(stderr, "The pipe given already exists.\n");
        return -1;
    }

    if (mkfifo(own_fifo_name, PIPE_PERMS) != 0) {
        PANIC("Can't create FIFO");
    }

    ssize_t total_wr = write(w_svfifo, sv_request, to_send);
    if (total_wr == -1) {
        pre_quit(own_fifo_name);
        PANIC("Error writting to the server fifo.");
    }

    STATUS("Server request sent");

    close(w_svfifo);

    // blocks here until mbroker opens the pipe in read mode
    int w_fifo = open(own_fifo_name, O_WRONLY);
    if (w_fifo == -1) {
        pre_quit(own_fifo_name);
        ERROR("Error opening own FIFO.");
        return -1;
    }
    signal(SIGPIPE, SIG_IGN);

    // buffer para escrever a mensagem a mandar para a caixa
    size_t bsize = OPCODE_LEN + MESSAGE_MAX_LEN;
    uint8_t buffer[bsize];

    opcode = PUB_SENDMSG_SV;
    memcpy(buffer, &opcode, OPCODE_LEN);

    while (1) {
        char *msg = NULL;
        size_t msg_size = 0;
        // obter uma linha do stdin
        ssize_t total_rd = getline(&msg, &msg_size, stdin);
        if (total_rd <= 0) {
            STATUS("Closing (EOF reached)");
            break;
        }

        // If the message received from stdin is longer than MESSAGE_MAX_LEN,
        // then it will truncate it at that value
        unsigned int i = 0;
        for (; i < MESSAGE_MAX_LEN && i < total_rd && msg[i] != '\n'; i++) {
            buffer[i + OPCODE_LEN] = (uint8_t)msg[i];
        }

        memset(buffer + OPCODE_LEN + i, 0, bsize - OPCODE_LEN - i);

        total_wr = write(w_fifo, buffer, bsize);
        if (total_wr < bsize || errno == EPIPE) {
            signal(SIGPIPE, SIG_DFL);
            errno = 0;
            STATUS("Closing (The box was removed)");
            break;
        }
    }

    close(w_fifo);
    pre_quit(own_fifo_name);

    return 0;
}