#include "../fs/operations.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>

#define BUFFER_SIZE 2
#define MAX_FILE_LEN 1024

char const dest_path[] = "/written.txt";
char const fw_contents[] = "sou bueda fixe ehehe (:";
char fr_contents[MAX_FILE_LEN + 1] = "\0";

bool over = false;
size_t written = 0;
pthread_rwlock_t over_locker = PTHREAD_RWLOCK_INITIALIZER;

void * thread_read_fn(void * args) {
    char buffer[BUFFER_SIZE + 1];
    
    int fh = *(int *)args;

    while (1) {
        ssize_t num_read;
        assert(num_read = tfs_read(fh, buffer, BUFFER_SIZE) != -1);

        buffer[num_read] = '\0';
        strcat(fr_contents, buffer);

        pthread_rwlock_rdlock(&over_locker);
        // while have things to read, continue
        if (over && strlen(fr_contents) == written) {
            pthread_rwlock_unlock(&over_locker);
            break;
        }
        pthread_rwlock_unlock(&over_locker);
    }
    assert(tfs_close(fh) != -1);
    return NULL;
}

void * thread_write_fn(void * args) {
    int fh = *(int *)args;

    size_t offset = 0;
    while (1) {
        pthread_rwlock_wrlock(&over_locker);
        ssize_t num_written = 0;
        assert(num_written = tfs_write(fh, fw_contents + offset, BUFFER_SIZE) >= 0);

        // number of bytes already written
        written = (offset += (size_t) num_written);

        // continue the writting while didnt write all the string
        if (offset >= strlen(fw_contents) || num_written == 0) {
            over = true;
            pthread_rwlock_unlock(&over_locker);
            break;
        }
        pthread_rwlock_unlock(&over_locker);
    }
    assert(tfs_close(fh) != -1);
    return NULL;
}

int main() {

    // init TécnicoFS
    tfs_params params = tfs_default_params();
    params.max_inode_count = 2; //root + file
    params.max_block_count = 2; //root + file
    params.max_open_files_count = 2;
    assert(tfs_init(&params) != -1);

    {
        pthread_t t[2];

        // Create file and open with different file handlers
        int fw = tfs_open(dest_path, TFS_O_CREAT | TFS_O_TRUNC);
        int fr = tfs_open(dest_path, 0);


        pthread_create(&t[0], NULL, thread_read_fn, &fr);
        pthread_create(&t[1], NULL, thread_write_fn, &fw);

        pthread_join(t[0], NULL);
        pthread_join(t[1], NULL);

        // compare the string read with the string written which must be the same
        assert(!strcmp(fr_contents, fw_contents));
    }

    // destroy TécnicoFS
    assert(tfs_destroy() != -1);

    printf("Successful test.\n");

    return 0;
}