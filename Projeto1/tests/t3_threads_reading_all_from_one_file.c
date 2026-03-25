#include "../fs/operations.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>

#define N_THREADS 20
#define BUFFER_SIZE 2

char const file_contents[] = "sou bueda fixe ehehe (:";
char const target_path[] = "/f1";
char const extern_path[] = "tests/extern_file.txt";
char const link_path1[] = "/l1";
char const fout_path[] = "tests/f.output";
char **strs;
int size = 0;
pthread_mutex_t size_lock = PTHREAD_MUTEX_INITIALIZER;

void write_contents(char const *path) {
    int f = tfs_open(path, TFS_O_TRUNC);
    assert(f != -1);

    assert(tfs_write(f, file_contents, sizeof(file_contents)) ==
           sizeof(file_contents));

    assert(tfs_close(f) != -1);
}

void print_contents(char const *path) {
    int f = tfs_open(path, 0);
    assert(f != -1);

    char buffer[BUFFER_SIZE + 1];
    memset(buffer, 0, BUFFER_SIZE + 1);
    ssize_t num_read;
    printf("\n***** file reading: %s *****\n", path);
    while ((num_read = tfs_read(f, buffer, BUFFER_SIZE))) {
        buffer[num_read] = '\0';
        printf("%s", buffer);
    }
    printf("\n***** end file reading *****\n\n");

    assert(tfs_close(f) != -1);
}

typedef struct _Args1_ {
    int nt;
    char path[MAX_FILE_NAME];
} args1;

void * thread_open_read_fn(void * a) {
    args1 *inp = (args1 *)a;
    
    int f = tfs_open(inp->path, 0);
    assert(f != -1);
    
    char buffer[BUFFER_SIZE + 1];
    memset(buffer, 0, BUFFER_SIZE + 1);
    ssize_t num_read;

    // printf("\n***** thread num: %d | file reading: %s *****\n",
    //     inp->nt, inp->path);

    pthread_mutex_lock(&size_lock);
    int i = size++;
    pthread_mutex_unlock(&size_lock);

    sleep(1);

    strs[i] = (char *) malloc((strlen(file_contents) + 1) * sizeof(char));
    strs[i][0] = '\0';
    while ((num_read = tfs_read(f, buffer, BUFFER_SIZE))) {
        assert(num_read != -1);

        strncat(strs[i], buffer, strlen(file_contents) - strlen(strs[i]));
    }

    sleep(1);

    //printf("\n***** end thread %d *****\n\n", inp->nt);
    // printf("* %6d", inp->nt);

    // if (inp->nt %10 == 0) printf("*\n");

    assert(tfs_close(f) != -1);

    return NULL;
}

typedef struct _Args2_ {
    char *sread;
    int fh;
    pthread_mutex_t *ms;
    int nt;
} args2;

void * thread_read_fn(void * a) {
    args2 *inp = (args2 *)a;

    int fh = inp->fh;
    char *sr = inp->sread;
    pthread_mutex_t *ms = inp->ms;

    char buffer[BUFFER_SIZE + 1];
    memset(buffer, 0, BUFFER_SIZE + 1);
    ssize_t num_read;

    pthread_mutex_lock(ms);
    num_read = tfs_read(fh, buffer, BUFFER_SIZE);
    while (num_read) {
        assert(num_read != -1);

        strncat(sr, buffer, strlen(file_contents) - strlen(sr));

        // printf("\n%d\n", inp->nt);
        pthread_mutex_unlock(ms);
        sleep(1);
        pthread_mutex_lock(ms);
        num_read = tfs_read(fh, buffer, BUFFER_SIZE);
    }

    pthread_mutex_unlock(ms);

    return NULL;
}

bool verify_t_reads() {
    for (int i = 1; i < N_THREADS; i++) {
        //printf("(start)%s(vs)%s(over)\n", strs[i-1], strs[i]);
        if (strcmp(strs[i-1], strs[i])) return false;
    }
    return true;
}

void free_strs() {
    for (int i = 0; i < N_THREADS; i++) free(strs[i]);
    free(strs);
}

int main() {

    // init TécnicoFS
    tfs_params params = tfs_default_params();
    params.max_inode_count = 2; //root + file
    params.max_block_count = 2; //root + file
    params.max_open_files_count = N_THREADS;
    assert(tfs_init(&params) != -1);

    // create file from extern file and prints its content
    {

        assert(tfs_copy_from_external_fs(extern_path, target_path) == 0);
        int f1 = tfs_open(target_path, 0);
        assert(f1 != -1);
        assert(tfs_close(f1) != -1);
        //print_contents(target_path);
    }

    // write the content in the file and prints it
    {
        int f1 = tfs_open(target_path, 0);
        assert(f1 != -1);
        write_contents(target_path);
        assert(tfs_close(f1) != -1);
        //print_contents(target_path);
    }

    // creates N_THREADS to open and read the same file
    // the expected behaviour is that each thread reads all the file, reading
    // all the same bytes at the same time. We can confirm this "paralelism"
    // because we have two sleep(1) inside the thread function and N_THREADS
    // threads and instead of it take it N_THREADS * 2 * 1 seconds to return
    // from the threads it only takes around 2 * 1 seconds.
    {   
        pthread_t t[N_THREADS];
        args1 a[N_THREADS];
        size = 0;
        strs = (char **) malloc(N_THREADS * sizeof(char *));
        for (int i = 0; i < N_THREADS; i++) {
            a[i].nt = i + 1;
            strcpy(a[i].path, target_path);
            assert(pthread_create(&t[i], NULL, thread_open_read_fn, &a[i]) == 0);
        }

        for (int i = 0; i < N_THREADS; i++) {
            pthread_join(t[i], NULL);
        }

        assert(verify_t_reads());

        free_strs();
    }

    // creates N_THREADS to read a file from the same file handle
    // the expected behaviour is that each thread reads certain quantity of
    // bytes and update the offset so that the other threads continue the read
    // from the updated offset
    {   
        size = 0;
        pthread_t t[N_THREADS];
        args2 a[N_THREADS];
        char read_s[sizeof(char)*6*(strlen(file_contents) + 1)];
        read_s[0] = '\0';
        int fh = tfs_open(target_path, 0);
        pthread_mutex_t msr = PTHREAD_MUTEX_INITIALIZER;
        for (int i = 0; i < N_THREADS; i++) {
            a[i].fh = fh;
            a[i].sread = read_s;
            a[i].ms = &msr;
            a[i].nt = i + 1;
            assert(pthread_create(&t[i], NULL, thread_read_fn, &a[i]) == 0);
        }
        
        for (int i = 0; i < N_THREADS; i++) {
            pthread_join(t[i], NULL);
        }

        assert(!strcmp(read_s, file_contents));
    }

    // destroy TécnicoFS
    assert(tfs_destroy() != -1);

    printf("Successful test.\n");

    return 0;
}