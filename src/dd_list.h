#ifndef DD_LIST_H_
#define DD_LIST_H_

#include <stdbool.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "../FreeRTOS_Source/include/task.h"

typedef enum task_type {
	PERIODIC,
	APERIODIC
} task_type_t;

typedef struct dd_task {
	TaskHandle_t t_handle;
	task_type_t type;
	uint8_t task_id;
	TickType_t release_time;
	TickType_t absolute_deadline;
	TickType_t completion_time;
} dd_task_t;

typedef struct dd_list_node {
	dd_task_t task;
	struct dd_list_node* next;
} list_node_t;

list_node_t* create_list_node(dd_task_t task);
void insert_task(list_node_t** head, dd_task_t task);
bool remove_task(list_node_t** head, uint8_t task_id);
dd_task_t* find_task(list_node_t* head, uint8_t task_id);
uint16_t get_list_size(list_node_t* head);
void delete_list(list_node_t** head);

#endif // DD_LIST_H_
