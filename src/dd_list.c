#include <dd_list.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>


/* Creates a new list node from a dd_task_t */
list_node_t* create_list_node(dd_task_t task) {
    list_node_t* new_node = (list_node_t*)pvPortMalloc(sizeof(list_node_t));
    if (new_node != NULL) {
        new_node->task = task;
        new_node->next = NULL;
    }
    return new_node;
}

/* Insert a new task into the list , preserving the ordering by
*  absolute deadline */
void insert_task(list_node_t** head, dd_task_t task) {
    list_node_t* new_node = create_list_node(task);
    if (new_node == NULL) {
        printf("ERROR: Could not allocate space for new list node.\n");
        return;
    }
    if (*head == NULL || (*head)->task.absolute_deadline > new_node->task.absolute_deadline) {
        new_node->next = *head;
        *head = new_node;
    } else {
        list_node_t* current = *head;
        while (current->next != NULL && current->next->task.absolute_deadline < new_node->task.absolute_deadline) {
            current = current->next;
        }
        new_node->next = current->next;
        current->next = new_node;
    }
}

/* Removes a node from the list, by task_id */
bool remove_task(list_node_t** head, uint8_t task_id) {
    if (*head == NULL) {
        return false;
    }
    if ((*head)->task.task_id == task_id) {
        list_node_t* temp = *head;
        *head = (*head)->next;
        vPortFree(temp);
        return true;
    }

    list_node_t* current = *head;
    while (current->next != NULL && current->next->task.task_id != task_id) {
        current = current->next;
    }

    if (current->next == NULL) {
        return false;
    }

    list_node_t* temp = current->next;
    current->next = current->next->next;
    vPortFree(temp);
    return true;
}

dd_task_t* find_task(list_node_t* head, uint8_t task_id) {
    list_node_t* current = head;
    while (current != NULL) {
        if (current->task.task_id == task_id) {
            return &current->task;
        }
        current = current->next;
    }
    return NULL;
}

uint16_t get_list_size(list_node_t* head) {
    uint16_t size = 0;
    list_node_t* current = head;
    while (current != NULL) {
        size++;
        current = current->next;
    }
    return size;
}

void delete_list(list_node_t** head) {
    while (*head != NULL) {
        list_node_t* temp = *head;
        *head = (*head)->next;
        vPortFree(temp);
    }
}
