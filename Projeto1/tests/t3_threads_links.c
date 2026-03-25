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
#define PATH_LEN 3

#define N_SYM_LINKS 6
#define N_HARD_LINKS 3

char const src_path[] = "tests/file_to_test_links.txt";
char const target_path[] = "/f1";

const char slink[3][PATH_LEN + 1] = {"/s1", "/s2", "/s3"};
const char hlink[3][PATH_LEN + 1] = {"/h1", "/h2", "/h3"};
const char slink_nested[3][PATH_LEN + 1] = {"/n1", "/n2", "/n3"};

char sym_link_target[] = "/f1";
pthread_mutex_t slt_locker = PTHREAD_MUTEX_INITIALIZER;

bool compare_content(const char *path1, const char* path2) {
    int f1 = tfs_open(path1, 0), f2 = tfs_open(path2, 0);

    if (f1 == -1 || f2 == -1) {
        return false;
    }

    char b1[MAX_FILE_LEN + 1];
    char b2[MAX_FILE_LEN + 1];

    ssize_t nread1 = tfs_read(f1, b1, MAX_FILE_LEN), nread2 = tfs_read(f2, b2, MAX_FILE_LEN);

    if (nread1 == -1 || nread2 == -1 || nread1 != nread2) {
        return false;
    }

    if (tfs_close(f1) == -1 || tfs_close(f2) == -1) {
        return false;
    }

    return !memcmp(b1, b2, (size_t)nread1);
}

typedef struct _Args_ {
    char nl[PATH_LEN + 1];
    char sl[PATH_LEN + 1];
    char hl[PATH_LEN + 1];
} args;

void * thread_link_fn(void *a) {
    args *ln = (args *) a;
    sleep(1);
    assert(tfs_link(target_path, ln->hl) != -1);

    return NULL;
}

void * thread_sym_link_fn(void *a) {
    args *ln = (args *) a;
    sleep(1);
    assert(tfs_sym_link(target_path, ln->sl) != -1);

    return NULL;
}

void * thread_sym_link_nested_fn(void *a) {
    args *ln = (args *) a;

    sleep(1);

    pthread_mutex_lock(&slt_locker);

    assert(tfs_sym_link(sym_link_target, ln->nl) != -1);

    strcpy(sym_link_target, ln->nl);

    pthread_mutex_unlock(&slt_locker);

    return NULL;
}

void * thread_unlink_fn(void *a) {
    char *ln = (char *) a;
    sleep(1);
    assert(tfs_unlink(ln) != -1);

    return NULL;
}

int main() {

    // init TécnicoFS
    tfs_params params = tfs_default_params();
    params.max_inode_count = 1 + 1 + N_SYM_LINKS;
    params.max_block_count = 1 + 1 + N_SYM_LINKS;
    params.max_open_files_count = N_SYM_LINKS + 1 + N_HARD_LINKS;
    assert(tfs_init(&params) != -1);

    // Testar se os links criados apontam todos para o mesmo conteúdo, se ao
    // dar unlink do target os sym links não funcionam mas sim os hard links e
    // que é impossível apagar um ficheiro ainda aberto
    {
        int size = 3*(N_HARD_LINKS + N_SYM_LINKS), ti = 0;
        pthread_t t[size];

        assert(tfs_copy_from_external_fs(src_path, target_path) != -1);

        args a[3];
        for (int i = 0; i < 3; i++) {
            strcpy(a[i].nl, slink_nested[i]);
            strcpy(a[i].hl, hlink[i]);
            strcpy(a[i].sl, slink[i]);

            pthread_create(&t[ti++], NULL, thread_sym_link_nested_fn, &a[i]);
            pthread_create(&t[ti++], NULL, thread_link_fn, &a[i]);
            pthread_create(&t[ti++], NULL, thread_sym_link_fn, &a[i]);
        }
        sleep(1);
        for (int i = 0; i < N_HARD_LINKS + N_SYM_LINKS; i++)
            pthread_join(t[i], NULL);

        // see if it is all pointing to the same content
        for (int i = 0; i < 3; i++) {
            assert(compare_content(target_path, slink_nested[i]));
            assert(compare_content(target_path, hlink[i]));
            assert(compare_content(target_path, slink[i]));
        }

        int f;
        assert((f = tfs_open(target_path, 0)) != -1);
        char s1[PATH_LEN + 1], s2[PATH_LEN + 1];
        strcpy(s1, hlink[0]);
        strcpy(s2, hlink[1]);
        pthread_create(&t[ti], NULL, thread_unlink_fn, s1);
        pthread_create(&t[ti+1], NULL, thread_unlink_fn, s2);
        sleep(1);
        pthread_join(t[ti], NULL);
        pthread_join(t[ti+1], NULL);
        assert(tfs_close(f) != -1);

        //sym links still work because target-path is still in the root directory
        for (int i = 0; i < 3; i++) {
            assert((f = tfs_open(slink_nested[i], 0)) != -1);
            assert(tfs_close(f) != -1);
            assert((f = tfs_open(slink[i], 0)) != -1);
            assert(tfs_close(f) != -1);
        }

        assert((f = tfs_open(hlink[2], 0)) != -1);
        assert(tfs_close(f) != -1);

        assert((f = tfs_open(target_path, 0)) != -1);
        assert(tfs_close(f) != -1);

        assert(tfs_unlink(target_path) != -1);
        
        //hard link still works (last hard link for this file)
        assert((f = tfs_open(hlink[2], 0)) != -1);
        assert(tfs_close(f) != -1);

        // sym links broken
        for (int i = 0; i < 3; i++) {
            assert((f = tfs_open(slink_nested[i], 0)) == -1);
            assert((f = tfs_open(slink[i], 0)) == -1);
        }

        // não se pode eliminar completamente ficheiros que estão abertos
        // hlink[2] é o último hard link para o ficheiro
        assert((f = tfs_open(hlink[2], 0)) != -1);
        assert(tfs_unlink(hlink[2]) == -1);
        assert(tfs_close(f) != -1);
        assert(tfs_unlink(hlink[2]) != -1);

        for (int i = 0; i < 3; i++) {
            assert(tfs_unlink(slink_nested[i]) != -1);
            assert(tfs_unlink(slink[i]) != -1);
        }
    }

    // destroy TécnicoFS
    assert(tfs_destroy() != -1);

    printf("Successful test.\n");

    return 0;
}