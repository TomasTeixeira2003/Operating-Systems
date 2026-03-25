#include <stdint.h>
#include <string.h>
#include <unistd.h>

#ifndef MBROKER_H
#define MBROKER_H

#define PIPE_PERMS (S_IRUSR | S_IWUSR | S_IRGRP) // 0640 in octal

#define OPCODE_LEN (sizeof(uint8_t))
#define RET_CODE_LEN (sizeof(int32_t))
#define LAST_BIT_LEN (sizeof(uint8_t))
#define BOX_SIZE_LEN (sizeof(uint64_t))
#define NUM_PUB_LEN (sizeof(uint64_t))
#define NUM_SUB_LEN (sizeof(uint64_t))

#define PIPE_NAME_MAX_LEN (256)
#define BOX_NAME_MAX_LEN (32)
#define MESSAGE_MAX_LEN (1024)
#define ERR_MESSAGE_MAX_LEN (MESSAGE_MAX_LEN)
// #define REQUEST_MAX_LEN (PIPE_NAME_MAX_LEN + BOX_NAME_MAX_LEN)
// #define ANSWER_MAX_LEN (RET_CODE_LEN + ERR_MESSAGE_MAX_LEN)
// #define POST_MAX_LEN (MESSAGE_MAX_LEN)
// #define RDPOST_MAX_LEN (MESSAGE_MAX_LEN)

#define BOX_MAX_NUM (64)
#define MSG_MAX_NUM_BOX (1024)
#define NUM_WORKER_THREADS (40)

#define OK_RET_CODE 0
#define ERR_RET_CODE -1

#define ERR_MSG_BOX_EXISTS "Já existe uma caixa com o nome dado."
#define ERR_MSG_BOX_NOT_FOUND "Não existe uma caixa com o nome dado."
#define ERR_MSG_CANT_CREATE "Não é possível adicionar a caixa."
#define ERR_MSG_CANT_REMOVE "Não é possível remover a caixa."
#define ERR_MSG_EMPTY_BOX_NAME "Não é permitido nomes de caixa vazios."
#define ERR_NO_BOXES_FOUND "NO BOXES FOUND"

#define NOT_LAST_BIT (0)
#define LAST_BIT (1)

#define HAS_PUB (1)
#define NO_PUB (0)

#define PUB_REG (1)         // request
#define SUB_REG (2)         // request
#define BOX_CRT (3)         // request
#define BOX_CRT_ANS (4)     // answer
#define BOX_REM (5)         // request
#define BOX_REM_ANS (6)     // answer
#define BOX_LST (7)         // request
#define BOX_LST_ANS (8)     // answer
#define PUB_SENDMSG_SV (9)  // post
#define SV_SENDMSG_SUB (10) // rdpost

#define FIRST_OPCODE (PUB_REG)
#define LAST_OPCODE (SV_SENDMSG_SUB)

#define max(x, y) (((x) > (y)) ? (x) : (y))
#define min(x, y) (((x) < (y)) ? (x) : (y))

#define rd_inc(dest, src_ptr, size)                                            \
    {                                                                          \
        memcpy((dest), (src_ptr), (size));                                     \
        (src_ptr) += (size);                                                   \
    }

#define wr_inc(dest_ptr, src, size)                                            \
    {                                                                          \
        memcpy((dest_ptr), (src), (size));                                     \
        (dest_ptr) += (size);                                                  \
    }

#define get_fifo_sufix(sufix_str)                                              \
    {                                                                          \
        pid_t pid = getpid();                                                  \
        int pid_len = snprintf(NULL, 0, "%d", pid);                            \
        if (pid_len > 0) {                                                     \
            sufix_str =                                                        \
                (char *)malloc(sizeof(char) * (strlen("-") + (size_t)pid_len + \
                                               strlen(".pipe") + 1));          \
            sprintf(sufix_str, "-%d", pid);                                    \
            strcat(sufix_str, ".pipe");                                        \
        } else {                                                               \
            sufix_str = (char *)malloc(sizeof(char) * 1);                      \
            strcpy(sufix_str, "");                                             \
        }                                                                      \
    }

#define inf(inst)                                                              \
    INFO("ANTES");                                                             \
    inst INFO("DEPOIS");

#define pre_quit(fifo_name)                                                    \
    {                                                                          \
        if (unlink(fifo_name) != 0)                                            \
            PANIC("Error unlinking FIFO.");                                    \
    }

typedef struct _binfo_ {
    char name[BOX_NAME_MAX_LEN + 1];
    uint64_t size;
    uint64_t n_pubs;
    uint64_t n_subs;
} Binfo;

#endif