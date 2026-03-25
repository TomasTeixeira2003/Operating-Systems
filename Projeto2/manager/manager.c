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

static void print_usage() {
    fprintf(stderr,
            "usage: \n"
            "   manager <register_pipe_name> <pipe_name> create <box_name>\n"
            "   manager <register_pipe_name> <pipe_name> remove <box_name>\n"
            "   manager <register_pipe_name> <pipe_name> list\n");
}

int compare_boxes(const void *a, const void *b) {
    Binfo *b1 = (Binfo *)a;
    Binfo *b2 = (Binfo *)b;
    return strcmp(b1->name, b2->name);
}

int main(int argc, char **argv) {
    if (argc != 4 && argc != 5) {
        print_usage();
        return -1;
    }

    if (strlen(argv[1]) > PIPE_NAME_MAX_LEN) {
        fprintf(stderr, "Pipe name size should be lesser than %d\n",
                PIPE_NAME_MAX_LEN);
        print_usage();
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
        print_usage();
        return -1;
    }
    char own_fifo_name[PIPE_NAME_MAX_LEN + 1];
    memset(own_fifo_name, 0, PIPE_NAME_MAX_LEN + 1);
    strcpy(own_fifo_name, argv[2]);
    strcat(own_fifo_name, sufix_str);
    free(sufix_str);

    if (argc == 5) {
        if (strlen(argv[4]) > BOX_NAME_MAX_LEN) {
            fprintf(stderr, "Box name size should be less or equl to %d\n",
                    BOX_NAME_MAX_LEN);
        }
        if (strcmp(argv[3], "create") && strcmp(argv[3], "remove")) {
            print_usage();
            return -1;
        }
    } else if (strcmp(argv[3], "list")) {
        print_usage();
        return -1;
    }

    // Open mbroker's pipe in write mode
    int w_svfifo = open(reg_fifo_name, O_WRONLY);
    if (w_svfifo == -1) {
        PANIC("Error opening register FIFO.");
        return -1;
    }

    size_t to_send;
    uint8_t *sv_request = NULL, *sv_req_ptr, opcode;

    opcode = BOX_REM; // by default
    char cmd = argv[3][0];

    switch (cmd) {
    case 'c':
        opcode = BOX_CRT; // changes the default opcode
        // fall through
    case 'r': {
        char box_name[BOX_NAME_MAX_LEN + 1];
        strcpy(box_name, argv[4]);

        to_send = OPCODE_LEN + PIPE_NAME_MAX_LEN + BOX_NAME_MAX_LEN;
        sv_request = (uint8_t *)malloc(sizeof(uint8_t) * to_send);
        if (sv_request == NULL) {
            return -1;
        }

        sv_req_ptr = sv_request;

        wr_inc(sv_req_ptr, &opcode, OPCODE_LEN);
        wr_inc(sv_req_ptr, own_fifo_name, PIPE_NAME_MAX_LEN);
        wr_inc(sv_req_ptr, box_name, BOX_NAME_MAX_LEN);
    } break;
    case 'l': {
        to_send = OPCODE_LEN + PIPE_NAME_MAX_LEN;

        sv_request = (uint8_t *)malloc(sizeof(uint8_t) * to_send);
        if (sv_request == NULL) {
            return -1;
        }
        opcode = BOX_LST;

        sv_req_ptr = sv_request;

        wr_inc(sv_req_ptr, &opcode, OPCODE_LEN);
        wr_inc(sv_req_ptr, own_fifo_name, PIPE_NAME_MAX_LEN);
    } break;
    default:
        PANIC("cmd invalid");
        pre_quit(own_fifo_name);
    }

    // Checks if the pipe with the pipe name associated already exists
    if (access(own_fifo_name, F_OK) == 0) {
        fprintf(stderr, "The pipe %s already exists.\n", own_fifo_name);
        return -1;
    }

    if (mkfifo(own_fifo_name, PIPE_PERMS) != 0) {
        PANIC("Can't create FIFO");
    }

    STATUS("Server request sent");
    ssize_t total_wr = write(w_svfifo, sv_request, to_send);
    if (total_wr == -1) {
        pre_quit(own_fifo_name);
        PANIC("Can't write to FIFO.");
    }

    free(sv_request);

    close(w_svfifo);

    // blocks here until mbroker opens the pipe in read mode
    int r_fifo = open(own_fifo_name, O_RDONLY);
    if (r_fifo == -1) {
        pre_quit(own_fifo_name);
        PANIC("Error opening own FIFO.");
    }

    ssize_t total_rd = read(r_fifo, &opcode, OPCODE_LEN);

    if ((opcode == BOX_CRT_ANS && cmd == 'c') ||
        (opcode == BOX_REM_ANS && cmd == 'r')) {
        do {
            size_t to_read = RET_CODE_LEN + ERR_MESSAGE_MAX_LEN;
            uint8_t ans[to_read], *ans_ptr;

            total_rd = read(r_fifo, ans, to_read);
            if (total_rd != to_read) {
                ERROR("Error reading from pipe.");
                break;
            }

            ans_ptr = ans;

            int32_t ret_code;
            rd_inc(&ret_code, ans_ptr, RET_CODE_LEN);

            if (ret_code == OK_RET_CODE) {
                fprintf(stdout, "OK\n");
            } else if (ret_code == ERR_RET_CODE) {
                char msg[ERR_MESSAGE_MAX_LEN + 1];
                msg[ERR_MESSAGE_MAX_LEN] = 0;
                rd_inc(&msg, ans_ptr, ERR_MESSAGE_MAX_LEN);

                fprintf(stdout, "ERROR %s\n", msg);
            } else {
                char buffer[RET_CODE_LEN + MESSAGE_MAX_LEN + 1];
                buffer[RET_CODE_LEN + MESSAGE_MAX_LEN] = 0;
                memcpy(buffer, ans, RET_CODE_LEN + MESSAGE_MAX_LEN);
                strcat(buffer, "|");
                ERROR("Return code inválido : %s", buffer);
            }
        } while (0);
    } else if (opcode == BOX_LST_ANS && cmd == 'l') {
        size_t to_read = OPCODE_LEN + LAST_BIT_LEN + BOX_NAME_MAX_LEN +
                         BOX_SIZE_LEN + NUM_PUB_LEN + NUM_SUB_LEN;

        uint8_t last, ans[to_read], *ans_ptr;

        memcpy(ans, &opcode, OPCODE_LEN);

        Binfo boxes[BOX_MAX_NUM];
        size_t count = 0;
        do {
            ans_ptr = ans + OPCODE_LEN;

            // lê-se todos os bytes de uma vez e depois vai se colocando cada
            // byte na variável correspondente
            total_rd = read(r_fifo, ans_ptr, to_read - OPCODE_LEN);
            if (total_rd == 0) {
                break;
            }
            if (total_rd < to_read - OPCODE_LEN) {
                pre_quit(own_fifo_name);
                PANIC(
                    "Erro ao ler a mensangem de resposta à listagem da caixa");
            }

            rd_inc(&last, ans_ptr, LAST_BIT_LEN);

            boxes[count].name[BOX_NAME_MAX_LEN] = 0;
            rd_inc(boxes[count].name, ans_ptr, BOX_NAME_MAX_LEN);

            // no boxes - because box name can only be empty in the case there
            // are no boxes
            if (count == 0 && boxes[0].name[0] == 0) {
                fprintf(stdout, ERR_NO_BOXES_FOUND);
                putchar('\n');
                break;
            }

            rd_inc(&boxes[count].size, ans_ptr, BOX_SIZE_LEN);
            rd_inc(&boxes[count].n_pubs, ans_ptr, NUM_PUB_LEN);
            rd_inc(&boxes[count].n_subs, ans_ptr, NUM_SUB_LEN);

            count++;

            if (last == LAST_BIT)
                break;

            total_rd = read(r_fifo, ans, OPCODE_LEN);
            if (total_rd == 0) {
                pre_quit(own_fifo_name);
                PANIC("Unexpected EOF in pipe");
            }
            if (total_rd == -1) {
                pre_quit(own_fifo_name);
                PANIC(
                    "Erro ao ler a mensangem de resposta à listagem da caixa");
            }

            memcpy(&opcode, ans, OPCODE_LEN);
            if (opcode != BOX_LST_ANS) {
                pre_quit(own_fifo_name);
                PANIC("Wrong OPCODE : %d | expected : %d", opcode, BOX_LST_ANS);
            }
        } while (1);

        qsort(boxes, count, sizeof(Binfo), compare_boxes);

        for (int i = 0; i < count; i++) {
            fprintf(stdout, "%s %zu %zu %zu\n", boxes[i].name, boxes[i].size,
                    boxes[i].n_pubs, boxes[i].n_subs);
        }
    } else {
        ERROR("OP code recebido errado. A abortar...");
    }

    close(r_fifo);

    pre_quit(own_fifo_name);

    return 0;
}
