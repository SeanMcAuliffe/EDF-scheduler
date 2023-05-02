/* Deadline-Driven Scheduler */
 

/* Standard includes */
#include <dd_list.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "stm32f4_discovery.h"
#include "stm32f4xx.h"
#include "stm32f4xx_rcc.h"
#include "stm32f4xx_tim.h"
#include <stdarg.h>

#include "FreeRTOSConfig.h"
#include "../FreeRTOS_Source/include/FreeRTOS.h"
#include "../FreeRTOS_Source/include/queue.h"
#include "../FreeRTOS_Source/include/semphr.h"
#include "../FreeRTOS_Source/include/task.h"
#include "../FreeRTOS_Source/include/timers.h"
#include "../FreeRTOS_Source/include/event_groups.h"
/* -------------------------------------------------------------------------- */

/* Defines ------------------------------------------------------------------ */
#define TASK1_INDICATOR LED6 // Blue
#define TASK2_INDICATOR LED5 // Red
#define TASK3_INDICATOR LED4 // Green

#define NEW_TASK_BIT (1 << 0)
#define TASK_COMPLETE_BIT (1 << 1)
#define MONITOR_REQUEST_BIT (1 << 2)

#define NO_WAIT (TickType_t)0
#define HIGH_PRIORITY tskIDLE_PRIORITY + 1
#define LOW_PRIORITY tskIDLE_PRIORITY

#define TESTBENCH_1

#ifdef TESTBENCH_1
#define TASK1_ET_MS 95UL
#define TASK1_PR_MS 500UL
#define TASK2_ET_MS 150UL
#define TASK2_PR_MS 500UL
#define TASK3_ET_MS 250UL
#define TASK3_PR_MS 750UL
#endif

#ifdef TESTBENCH_2
#define TASK1_ET_MS 95UL
#define TASK1_PR_MS 250UL
#define TASK2_ET_MS 150UL
#define TASK2_PR_MS 500UL
#define TASK3_ET_MS 250UL
#define TASK3_PR_MS 750UL
#endif

#ifdef TESTBENCH_3
#define TASK1_ET_MS 100UL
#define TASK1_PR_MS 500UL
#define TASK2_ET_MS 200UL
#define TASK2_PR_MS 500UL
#define TASK3_ET_MS 200UL
#define TASK3_PR_MS 500UL
#endif
/* -------------------------------------------------------------------------- */

/* Typedefs ----------------------------------------------------------------- */
typedef enum dds_request {
	GET_ACTIVE, GET_COMPLETED, GET_OVERDUE
} dds_request_t;

typedef struct task_params {
	uint8_t task_id;
	TickType_t execution_time;
} task_params_t;
/* -------------------------------------------------------------------------- */

/* Helper Functions --------------------------------------------------------- */
uint16_t ticksToMS(TickType_t ticks) {
	return (uint16_t)(ticks * portTICK_PERIOD_MS);
}
/* -------------------------------------------------------------------------- */

/* RTOS Handles ------------------------------------------------------------- */
QueueHandle_t xnewTaskQueue = NULL;
QueueHandle_t xcompleteTaskQueue = NULL;
QueueHandle_t xDDSRequestQueue = NULL;
QueueHandle_t xDDSReplyQueue = NULL;

SemaphoreHandle_t xDDSSemaphore = NULL;
EventGroupHandle_t xDDSEventGroup = NULL;
/* -------------------------------------------------------------------------- */

/* Task Function Prototypes ------------------------------------------------- */
void vDDS_Task(void* pvParameters);
void vGenerator_Task(void* pvParameters);
void vMonitor_Task(void* pvParameters);
void vRunner_Task(void* pvParameters);
/* -------------------------------------------------------------------------- */

/* Main --------------------------------------------------------------------- */
int main() {

	STM_EVAL_LEDInit(TASK1_INDICATOR);
	STM_EVAL_LEDInit(TASK2_INDICATOR);
	STM_EVAL_LEDInit(TASK3_INDICATOR);

	uint8_t* gen1 = (uint8_t*) pvPortMalloc(sizeof(uint8_t));
	uint8_t* gen2 = (uint8_t*) pvPortMalloc(sizeof(uint8_t));
	uint8_t* gen3 = (uint8_t*) pvPortMalloc(sizeof(uint8_t));

	*gen1 = 1;
	*gen2 = 2;
	*gen3 = 3;

	xTaskCreate(vGenerator_Task, "Gen1", configMINIMAL_STACK_SIZE, (void*) gen1, 5, NULL);
	xTaskCreate(vGenerator_Task, "Gen2", configMINIMAL_STACK_SIZE, (void*) gen2, 4, NULL);
	xTaskCreate(vGenerator_Task, "Gen3", configMINIMAL_STACK_SIZE, (void*) gen3, 3, NULL);
	xTaskCreate(vMonitor_Task, "Monitor", configMINIMAL_STACK_SIZE, NULL, 2, NULL);
	xTaskCreate(vDDS_Task, "DDS", configMINIMAL_STACK_SIZE, NULL, 6, NULL);

	/* TODO: These queues might need to be longer */
	xDDSRequestQueue = xQueueCreate(1, sizeof(dds_request_t));
	xDDSReplyQueue = xQueueCreate(1, sizeof(list_node_t*));
	xnewTaskQueue = xQueueCreate(10, sizeof(dd_task_t));
	xcompleteTaskQueue = xQueueCreate(10, sizeof(uint8_t));

	vQueueAddToRegistry(xDDSRequestQueue, "Release Task Queue");
	vQueueAddToRegistry(xDDSReplyQueue, "Complete Task Queue");

	xDDSSemaphore = xSemaphoreCreateBinary();
	xDDSEventGroup = xEventGroupCreate();

	printf("Starting Scheduler ... \n");
	vTaskStartScheduler();

	return 0;
}
/* -------------------------------------------------------------------------- */

/* Deadline Driven Scheduler ------------------------------------------------ */
void vDDS_Task(void* pvParameters) {

	/* Internal data structures */
	list_node_t* active_head = NULL;
	list_node_t* complete_head = NULL;
	list_node_t* overdue_head = NULL;

	/* For storing data from incoming messages */
	TickType_t current_time;
	dds_request_t monitor_msg;
	uint8_t completed_task_id;

	/* Newly generated task */
	dd_task_t new_task;

	/* For searching through stored tasks */
	list_node_t* current_node = NULL;
	dd_task_t* task_ptr = NULL;

	/* Synchronization with system events */
	BaseType_t rc = pdFALSE;
	EventBits_t uxBits;

	EventBits_t bit_group = (NEW_TASK_BIT | TASK_COMPLETE_BIT | MONITOR_REQUEST_BIT);

	//printf("DDS: Start\n");

	while (1) {

		/* Suspend the DDS until a new event wakes it up */
		uxBits = xEventGroupWaitBits(xDDSEventGroup, bit_group, pdTRUE, pdFALSE, portMAX_DELAY);

		current_time = xTaskGetTickCount();

		/* Check if a new task has been released */
		if (uxBits & NEW_TASK_BIT) {
			/* Retrieve the new task struct(s)
			 * and add it to the active list */
			rc = pdTRUE;
			while (rc == pdTRUE) {
				rc = xQueueReceive(xnewTaskQueue, &new_task, NO_WAIT);
				if (rc == pdTRUE) {
					new_task.release_time = xTaskGetTickCount();
					insert_task(&active_head, new_task);
					//printf("DDS: release %d\n", new_task.task_id);
				}
			}
		}

		/* Check if a task has been completed */
		if (uxBits & TASK_COMPLETE_BIT) {
			/* Remove task from active list, add to completed list */
			rc = pdTRUE;
			while (rc == pdTRUE) {
				rc = xQueueReceive(xcompleteTaskQueue, &completed_task_id, NO_WAIT);
				if (rc == pdTRUE) {
					task_ptr = find_task(active_head, completed_task_id);
					if (task_ptr == NULL) {
						printf("DDS: Failed to find completed task\n");
						continue;
					}
					task_ptr->completion_time = xTaskGetTickCount();
					if (current_time <= task_ptr->absolute_deadline) {
						insert_task(&complete_head, *task_ptr);
						//printf("DDS: %d complete\n", task_ptr->task_id);
					} else {
						insert_task(&overdue_head, *task_ptr);
						//printf("DDS: %d overdue\n", task_ptr->task_id);
					}
					vTaskDelete(task_ptr->t_handle);
					remove_task(&active_head, completed_task_id);
				}
			}
		}

		/* Possible add software timer to set 4th bit, to indicate that
		 * DDS should do an overdue check independent of other events */

		/* Redo schedule */
		if (uxBits & (NEW_TASK_BIT | TASK_COMPLETE_BIT)) {

			/* Check if there are any tasks in the active list */
			if (active_head == NULL) {
				//printf("DDS: No Tasks\n");
				continue;
			}

			/* Check for overdue tasks*/
			current_time = xTaskGetTickCount();
			current_node = active_head;
			while (current_node != NULL) {
				if (current_node->task.absolute_deadline < current_time) {
					// Remove from active, add to overdue
					printf("DDS: %d overdue!\n", current_node->task.task_id);
					new_task = current_node->task;
					remove_task(&active_head, current_node->task.task_id);
					insert_task(&overdue_head, new_task);
					vTaskDelete(new_task.t_handle); // Stop the late task
				} else {
					break; // No more overdue tasks
				}
				current_node = current_node->next;
			}

			/* Choose new task to run according to EDF */
			if (active_head != NULL) {
				vTaskPrioritySet(active_head->task.t_handle, HIGH_PRIORITY);
			}

			/* Set all non-head nodes to low priority */
			current_node = active_head->next;
			while (current_node != NULL) {
				vTaskPrioritySet(current_node->task.t_handle, LOW_PRIORITY);
				current_node = current_node->next;
			}
		}

		/* Check if the monitor has requested a task */
		if (uxBits & MONITOR_REQUEST_BIT) {
			/* Retrieve the request from the monitor */
			rc = xQueueReceive(xDDSRequestQueue, &monitor_msg, NO_WAIT);
			if (rc == pdTRUE) {
				switch (monitor_msg) {
				case GET_ACTIVE:
					xQueueSend(xDDSReplyQueue, &active_head, 1000);
					break;
				case GET_COMPLETED:
					xQueueSend(xDDSReplyQueue, &complete_head, 1000);
					break;
				case GET_OVERDUE:
					xQueueSend(xDDSReplyQueue, &overdue_head, 1000);
					break;
				default:
					break;
				}
			} else {
				printf("DDS: Failed to receive monitor request\n");
			}
		}
	}
}
/* -------------------------------------------------------------------------- */

/* DDS Interface Functions -------------------------------------------------- */
void release_dd_task(TaskHandle_t t_handle, task_type_t type, uint8_t task_id, TickType_t abs_deadline) {

	BaseType_t rc = pdFALSE;

	/* Construct a new dd_task_t struct */
	dd_task_t new_task;
	new_task.t_handle = t_handle;
	new_task.type = type;
	new_task.task_id = task_id;
	new_task.absolute_deadline = abs_deadline;

	rc = xQueueSend(xnewTaskQueue, &new_task, 1000);

	if (rc != pdTRUE) {
		/* Release Task Failed*/
		printf("ERR: RTF\n");
	}
}

void complete_dd_task(uint8_t task_id) {

	BaseType_t rc = pdFALSE;

	rc = xQueueSend(xcompleteTaskQueue, &task_id, 1000);

	if (rc != pdTRUE) {
		/* Complete Task Failed */
		printf("ERR: CTF\n");
	}
}

list_node_t* get_active_dd_task_list() {

	BaseType_t rc = pdFALSE;
	dds_request_t msg = GET_ACTIVE;
	list_node_t* activeListHead = NULL;
	rc = xQueueSend(xDDSRequestQueue, &msg, 1000);

	if (rc != pdTRUE) {
		/* Get Active to Request Queue Failed */
		printf("ERR: GARQF\n");
		xQueueReset(xDDSRequestQueue);
		return NULL;
	}

	xEventGroupSetBits(xDDSEventGroup, MONITOR_REQUEST_BIT);

	rc = xQueueReceive(xDDSReplyQueue, &activeListHead, pdMS_TO_TICKS(500));
	if (rc != pdTRUE) {
		/* Active Not Received */
		printf("ERR: ANR\n");
		xQueueReset(xDDSRequestQueue);
		return NULL;
	}
	return activeListHead;
}

list_node_t* get_complete_dd_task_list() {

	BaseType_t rc = pdFALSE;
	dds_request_t msg = GET_COMPLETED;
	list_node_t* completeListHead = NULL;
	rc = xQueueSend(xDDSRequestQueue, &msg, 1000);

	if (rc != pdTRUE) {
		/* Get Completed Request Q Failed */
		printf("ERR: GCRQF\n");
		xQueueReset(xDDSRequestQueue);
		return NULL;
	}

	xEventGroupSetBits(xDDSEventGroup, MONITOR_REQUEST_BIT);

	rc = xQueueReceive(xDDSReplyQueue, &completeListHead, pdMS_TO_TICKS(500));
	if (rc != pdTRUE) {
		/* Complete Not Received */
		printf("ERR: CNR\n");
		xQueueReset(xDDSRequestQueue);
		return NULL;
	}
	return completeListHead;
}

list_node_t* get_overdue_dd_task_list() {
	BaseType_t rc = pdFALSE;
	dds_request_t msg = GET_OVERDUE;
	list_node_t* overdueListHead = NULL;
	rc = xQueueSend(xDDSRequestQueue, &msg, 1000);

	if (rc != pdTRUE) {
		/* Get Overdue Request Q Failed */
		printf("ERROR: GORQF \n");
		xQueueReset(xDDSRequestQueue);
		return NULL;
	}

	xEventGroupSetBits(xDDSEventGroup, MONITOR_REQUEST_BIT);

	rc = xQueueReceive(xDDSReplyQueue, &overdueListHead, pdMS_TO_TICKS(500));
	if (rc != pdTRUE) {
		/* Overdue Not Received */
		printf("ERR: ONR\n");
		xQueueReset(xDDSRequestQueue);
		return NULL;
	}
	return overdueListHead;
}
/* -------------------------------------------------------------------------- */

/* User-Defined Task Generator ---------------------------------------------- */
void vGenerator_Task(void* pvParameters) {

	TickType_t execution_time;
	TickType_t abs_deadline;
	TickType_t current_time;
	uint8_t generator_id;
	uint32_t task_period;
	uint8_t task_id;

	/* SETUP: Determine the Generator ID and Task Period */
	generator_id = *((uint8_t*) pvParameters);
	switch (generator_id) {
	case 1:
		task_period = pdMS_TO_TICKS(TASK1_PR_MS);
		execution_time = pdMS_TO_TICKS(TASK1_ET_MS);
		task_id = 1;
		break;
	case 2:
		task_period = pdMS_TO_TICKS(TASK2_PR_MS);
		execution_time = pdMS_TO_TICKS(TASK2_ET_MS);
		task_id = 2;
		break;
	case 3:
		task_period = pdMS_TO_TICKS(TASK3_PR_MS);
		execution_time = pdMS_TO_TICKS(TASK3_ET_MS);
		task_id = 3;
		break;
	default:
		/* Generator Bad Test Bench Number */
		printf("ERR: GBTBN\n");
		return;
		break;
	}
	//printf("GEN %d: Start\n", generator_id);

	/* LOOP: Run the Generator */
	while (1) {

		current_time = xTaskGetTickCount();

		/* Allocate a new task handle */
		TaskHandle_t usr_task_handle;

		/* Allocate a new task parameter struct */
		task_params_t* task_params = (task_params_t*) pvPortMalloc(sizeof(task_params_t));
		task_params->task_id = task_id;
		task_params->execution_time = execution_time;

		xTaskCreate(vRunner_Task,
					"User Task",
					configMINIMAL_STACK_SIZE,
					(void*)task_params,
					LOW_PRIORITY,
					&usr_task_handle);

		if (usr_task_handle == NULL) {
			/* User Task Creation Failed */
			printf("ERR: UTCF %d.\n", task_id);
			continue;
		}

		abs_deadline = current_time + task_period;
		//printf("TSK %d DL %u\n", generator_id, ticksToMS(abs_deadline));

		release_dd_task(usr_task_handle, PERIODIC, task_id, abs_deadline);
		xEventGroupSetBits(xDDSEventGroup, NEW_TASK_BIT);
		vTaskDelay(task_period - (xTaskGetTickCount() - current_time));
	}
}
/* -------------------------------------------------------------------------- */

/* User-Defined Task Runner Task -------------------------------------------- */
void vRunner_Task(void* pvParameters) {

	//TaskHandle_t currentHandle = xTaskGetCurrentTaskHandle();
	//uint8_t indicator;
	uint8_t task_id;
	TickType_t execution_time;
	TickType_t runtime = 0;
	//TaskStatus_t task_status;
	task_params_t* task_params;

	TickType_t currentTick;
	TickType_t previousTick = 0;

	/* Initialization, to figure out which task I am */
	task_params = (task_params_t*) pvParameters;
	task_id = task_params->task_id;
	execution_time = task_params->execution_time;

	switch (task_id) {
	case 1:
		break;
	case 2:
		break;
	case 3:
		break;
	default:
		printf("ERR: RBTN\n"); // Runner Bad Task Number
		vTaskDelete(NULL);
		return;
	}

	//printf("RUN %d: Start\n", task_id);

	/* TODO: This measurement method may not work */
	while (runtime < execution_time) {
		currentTick = xTaskGetTickCount();
		if (currentTick > previousTick) {
			runtime++;
			previousTick = currentTick;
		}
	}

	/* Call complete_dd_task() and clean up,
	 * I will be deleted by the DDS */
	complete_dd_task(task_id);
	vPortFree((void*) task_params);
	xEventGroupSetBits(xDDSEventGroup, TASK_COMPLETE_BIT);
}
/* -------------------------------------------------------------------------- */

/* Monitor Task ------------------------------------------------------------- */
void vMonitor_Task(void* pvParameters) {

	list_node_t* active = NULL;
	list_node_t* complete = NULL;
	list_node_t* overdue = NULL;
	list_node_t* current = NULL;

	uint16_t size = 0;
	//printf("MON: Start\n");

	// Must run ~fast~, to not add overhead and cause missed deadlines.
	while (1) {

		vTaskDelay(pdMS_TO_TICKS(1000)); // shorter?

		active = get_active_dd_task_list();
		complete = get_complete_dd_task_list();
		overdue = get_overdue_dd_task_list();

		taskENTER_CRITICAL();
		printf("\n\nCurrent Time: %u\n", ticksToMS(xTaskGetTickCount()));
		size = get_list_size(active);
		printf("Active Tasks: %d\n", size);
		current = active;
		while (current != NULL) {
			printf("	Task %d, Release Time: %u Deadline %u\n",
					current->task.task_id,
					ticksToMS(current->task.release_time),
					ticksToMS(current->task.absolute_deadline));
			current = current->next;
		}
		size = get_list_size(complete);
		printf("Completed Tasks: %d\n", size);
		current = complete;
		while (current != NULL) {
			printf(
					"	Task: %d, Release Time: %u, Completion Time: %u, Deadline: %u\n",
					current->task.task_id,
					ticksToMS(current->task.release_time),
					ticksToMS(current->task.completion_time),
					ticksToMS(current->task.absolute_deadline));
			current = current->next;
		}
		size = get_list_size(overdue);
		printf("Overdue Tasks: %d\n", size);
		current = overdue;
		while (current != NULL) {
			printf("	Task %d, Release Time: %u, Deadline %u\n",
					current->task.task_id,
					ticksToMS(current->task.release_time),
					ticksToMS(current->task.absolute_deadline));
			current = current->next;
		}
		printf("\nCurrent Time: %u\n\n", ticksToMS(xTaskGetTickCount()));
		taskEXIT_CRITICAL();
	}
}
/* -------------------------------------------------------------------------- */

// timer setup
void TIM2_init(void) {
	TIM_TimeBaseInitTypeDef TIM_TimeBaseInitStruct;
	RCC_ClocksTypeDef RCC_Clocks;

	RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2, ENABLE);

	RCC_GetClocksFreq(&RCC_Clocks);

	TIM_TimeBaseInitStruct.TIM_Period = 0xFFFFFFFF;
	TIM_TimeBaseInitStruct.TIM_Prescaler = RCC_Clocks.PCLK1_Frequency / 1000000
			- 1;
	TIM_TimeBaseInitStruct.TIM_ClockDivision = 0;
	TIM_TimeBaseInitStruct.TIM_CounterMode = TIM_CounterMode_Up;
	TIM_TimeBaseInit(TIM2, &TIM_TimeBaseInitStruct);

	TIM_Cmd(TIM2, ENABLE);
}

uint32_t readTIM2(void) {
	return TIM_GetCounter(TIM2);
}

/* Boilerplate -------------------------------------------------------------- */
void vApplicationMallocFailedHook(void) {
	/* The malloc failed hook is enabled by setting
	 configUSE_MALLOC_FAILED_HOOK to 1 in FreeRTOSConfig.h.

	 Called if a call to pvPortMalloc() fails because there is insufficient
	 free memory available in the FreeRTOS heap.  pvPortMalloc() is called
	 internally by FreeRTOS API functions that create tasks, queues, software
	 timers, and semaphores.  The size of the FreeRTOS heap is set by the
	 configTOTAL_HEAP_SIZE configuration constant in FreeRTOSConfig.h. */
	printf("Malloc Failed\n");
	for (;;)
		;
}

void vApplicationStackOverflowHook( xTaskHandle pxTask, signed char *pcTaskName) {
	(void) pcTaskName;
	(void) pxTask;

	/* Run time stack overflow checking is performed if
	 configconfigCHECK_FOR_STACK_OVERFLOW is defined to 1 or 2.  This hook
	 function is called if a stack overflow is detected.  pxCurrentTCB can be
	 inspected in the debugger if the task name passed into this function is
	 corrupt. */
	for (;;)
		;
}

void vApplicationIdleHook(void) {
	volatile size_t xFreeStackSpace;

	/* The idle task hook is enabled by setting configUSE_IDLE_HOOK to 1 in
	 FreeRTOSConfig.h.

	 This function is called on each cycle of the idle task.  In this case it
	 does nothing useful, other than report the amount of FreeRTOS heap that
	 remains unallocated. */
	xFreeStackSpace = xPortGetFreeHeapSize();

	if (xFreeStackSpace > 100) {
		/* By now, the kernel has allocated everything it is going to, so
		 if there is a lot of heap remaining unallocated then
		 the value of configTOTAL_HEAP_SIZE in FreeRTOSConfig.h can be
		 reduced accordingly. */
	}
}

//static void prvSetupHardware( void )
//{
//	/* Ensure all priority bits are assigned as preemption priority bits.
//	http://www.freertos.org/RTOS-Cortex-M3-M4.html */
//	NVIC_SetPriorityGrouping( 0 );
//
//	/* TODO: Setup the clocks, etc. here, if they were not configured before
//	main() was called. */
//}
/* -------------------------------------------------------------------------- */
