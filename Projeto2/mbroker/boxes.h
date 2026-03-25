#include "mbroker.h"
#include "operations.h"
#include "stdbool.h"

#include <pthread.h>

#ifndef BOXES_H
#define BOXES_H

int box_search(char *box_name);
int box_add(char *box_name);
int box_rem(int box_index);
int box_lst(Binfo *arr, size_t size);
ssize_t box_write(int box_index, uint8_t *buffer, size_t size);
ssize_t box_read(int box_index, int sub_id, uint8_t *buffer, size_t size);
int box_register_pub(int box_index);
int box_unregister_pub(int box_index);
int box_register_sub(int box_index);
int box_unregister_sub(int box_index, int sub_id);
void box_notify_new_write(int box_index);
void box_wait_for_new_write(int box_index, int sub_id);

int init_boxes(size_t num_boxes, size_t box_capacity, size_t max_open);
void destroy_boxes();

#endif