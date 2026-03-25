/*******************************************************************************
 * ECE 455 - Project 2: Deadline-Driven Scheduler (DDS)
 *
 * Implements an Earliest Deadline First (EDF) scheduler on top of FreeRTOS
 * running on an STM32F4 Discovery board.
 *
 * NOTE: This version is compatible with heap_1.c (no vPortFree needed).
 * All list nodes come from a static pool, and user F-Tasks are pre-created
 * once then reused across periods (suspended/resumed, never deleted).
 ******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>

#include "stm32f4_discovery.h"

/* FreeRTOS includes */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "timers.h"

/*******************************************************************************
 * Configuration - Test Bench Selection
 * Uncomment ONE of the following to select a test bench.
 ******************************************************************************/
// #define TEST_BENCH_1
// #define TEST_BENCH_2
 #define TEST_BENCH_3

/*******************************************************************************
 * Test Bench Parameters
 ******************************************************************************/
#ifdef TEST_BENCH_1
    #define TASK1_EXEC_TIME   95
    #define TASK1_PERIOD      500
    #define TASK2_EXEC_TIME   150
    #define TASK2_PERIOD      500
    #define TASK3_EXEC_TIME   250
    #define TASK3_PERIOD      750
#endif

#ifdef TEST_BENCH_2
    #define TASK1_EXEC_TIME   95
    #define TASK1_PERIOD      250
    #define TASK2_EXEC_TIME   150
    #define TASK2_PERIOD      500
    #define TASK3_EXEC_TIME   250
    #define TASK3_PERIOD      750
#endif

#ifdef TEST_BENCH_3
    #define TASK1_EXEC_TIME   100
    #define TASK1_PERIOD      500
    #define TASK2_EXEC_TIME   200
    #define TASK2_PERIOD      500
    #define TASK3_EXEC_TIME   200
    #define TASK3_PERIOD      500
#endif

#define NUM_DD_TASKS          3

/* * CPU BURN CALIBRATION
 * Adjust this constant so that 1 ms of requested execution time exactly
 * matches 1 ms of physical wall-clock time when a task is not preempted.
 */
#define LOOPS_PER_MS          16000

/*******************************************************************************
 * Priority Levels
 ******************************************************************************/
#define PRIORITY_DDS          (configMAX_PRIORITIES - 1)
#define PRIORITY_GENERATOR    (configMAX_PRIORITIES - 2)
#define PRIORITY_USER_HIGH    (configMAX_PRIORITIES - 3)
#define PRIORITY_USER_LOW     (1)
#define PRIORITY_MONITOR      (1)

/*******************************************************************************
 * Static Node Pool
 * heap_1 has no vPortFree, so we manage our own fixed pool of list nodes.
 ******************************************************************************/
#define NODE_POOL_SIZE 30

/*******************************************************************************
 * Data Structures
 ******************************************************************************/

typedef enum { PERIODIC, APERIODIC } task_type;

typedef struct {
    TaskHandle_t t_handle;
    task_type    type;
    uint32_t     task_id;
    uint32_t     release_time;
    uint32_t     absolute_deadline;
    uint32_t     completion_time;
} dd_task;

typedef struct dd_task_node {
    dd_task               task;
    struct dd_task_node  *next;
    uint8_t               in_use;
} dd_task_list;

/* Message types for DDS queue communication */
typedef enum {
    MSG_RELEASE_TASK,
    MSG_COMPLETE_TASK,
    MSG_GET_ACTIVE_LIST,
    MSG_GET_COMPLETED_LIST,
    MSG_GET_OVERDUE_LIST
} dds_msg_type;

typedef struct {
    dds_msg_type  type;
    dd_task       task_info;
    uint32_t      task_id;
} dds_message;

/*******************************************************************************
 * Static Node Pool Implementation
 ******************************************************************************/
static dd_task_list node_pool[NODE_POOL_SIZE];

static void node_pool_init(void) {
    int i;
    for (i = 0; i < NODE_POOL_SIZE; i++) {
        node_pool[i].in_use = 0;
        node_pool[i].next = NULL;
    }
}

static dd_task_list *node_alloc(void) {
    int i;
    for (i = 0; i < NODE_POOL_SIZE; i++) {
        if (!node_pool[i].in_use) {
            node_pool[i].in_use = 1;
            node_pool[i].next = NULL;
            return &node_pool[i];
        }
    }
    return NULL;
}

static void node_free(dd_task_list *node) {
    if (node != NULL) {
        node->in_use = 0;
        node->next = NULL;
    }
}

/*******************************************************************************
 * Global Handles
 ******************************************************************************/
static QueueHandle_t     dds_queue;
static QueueHandle_t     response_queue;
static SemaphoreHandle_t print_mutex;

static TimerHandle_t gen_timers[NUM_DD_TASKS];
static TaskHandle_t  user_task_handles[NUM_DD_TASKS];

static const uint32_t exec_times[NUM_DD_TASKS] = {
    TASK1_EXEC_TIME, TASK2_EXEC_TIME, TASK3_EXEC_TIME
};
static const uint32_t task_periods[NUM_DD_TASKS] = {
    TASK1_PERIOD, TASK2_PERIOD, TASK3_PERIOD
};

static uint32_t next_task_id = 0;

/* Per-task-type: which dd-task id is currently assigned */
static volatile uint32_t current_task_id[NUM_DD_TASKS];

/*******************************************************************************
 * Safe Printf - thread-safe via mutex
 ******************************************************************************/
static int safe_printf(const char *format, ...) {
    va_list args;
    int ret;
    va_start(args, format);
    xSemaphoreTake(print_mutex, portMAX_DELAY);
    ret = vprintf(format, args);
    xSemaphoreGive(print_mutex);
    va_end(args);
    return ret;
}

/*******************************************************************************
 * List Helper Functions (static node pool)
 ******************************************************************************/

static void list_insert_sorted(dd_task_list **head, dd_task task) {
    dd_task_list *new_node = node_alloc();
    if (new_node == NULL) {
        safe_printf("ERROR: Node pool exhausted!\n");
        return;
    }
    new_node->task = task;

    if (*head == NULL || task.absolute_deadline < (*head)->task.absolute_deadline) {
        new_node->next = *head;
        *head = new_node;
    } else {
        dd_task_list *current = *head;
        while (current->next != NULL &&
               current->next->task.absolute_deadline <= task.absolute_deadline) {
            current = current->next;
        }
        new_node->next = current->next;
        current->next = new_node;
    }
}

static int list_remove_by_id(dd_task_list **head, uint32_t task_id,
                             dd_task *removed_task) {
    dd_task_list *current = *head;
    dd_task_list *prev = NULL;

    while (current != NULL) {
        if (current->task.task_id == task_id) {
            if (prev == NULL) {
                *head = current->next;
            } else {
                prev->next = current->next;
            }
            *removed_task = current->task;
            node_free(current);
            return 1;
        }
        prev = current;
        current = current->next;
    }
    return 0;
}

static uint32_t list_count(dd_task_list *head) {
    uint32_t count = 0;
    while (head != NULL) {
        count++;
        head = head->next;
    }
    return count;
}

/*******************************************************************************
 * DDS Interface Functions
 ******************************************************************************/

void release_dd_task(TaskHandle_t t_handle, task_type type,
                     uint32_t task_id, uint32_t absolute_deadline) {
    dds_message msg;
    msg.type = MSG_RELEASE_TASK;
    msg.task_info.t_handle          = t_handle;
    msg.task_info.type              = type;
    msg.task_info.task_id           = task_id;
    msg.task_info.absolute_deadline = absolute_deadline;
    msg.task_info.release_time      = 0;
    msg.task_info.completion_time   = 0;
    xQueueSend(dds_queue, &msg, portMAX_DELAY);
}

void complete_dd_task(uint32_t task_id) {
    dds_message msg;
    msg.type    = MSG_COMPLETE_TASK;
    msg.task_id = task_id;
    xQueueSend(dds_queue, &msg, portMAX_DELAY);
}

dd_task_list *get_active_dd_task_list(void) {
    dds_message msg;
    dd_task_list *list = NULL;
    msg.type = MSG_GET_ACTIVE_LIST;
    xQueueSend(dds_queue, &msg, portMAX_DELAY);
    xQueueReceive(response_queue, &list, portMAX_DELAY);
    return list;
}

dd_task_list *get_completed_dd_task_list(void) {
    dds_message msg;
    dd_task_list *list = NULL;
    msg.type = MSG_GET_COMPLETED_LIST;
    xQueueSend(dds_queue, &msg, portMAX_DELAY);
    xQueueReceive(response_queue, &list, portMAX_DELAY);
    return list;
}

dd_task_list *get_overdue_dd_task_list(void) {
    dds_message msg;
    dd_task_list *list = NULL;
    msg.type = MSG_GET_OVERDUE_LIST;
    xQueueSend(dds_queue, &msg, portMAX_DELAY);
    xQueueReceive(response_queue, &list, portMAX_DELAY);
    return list;
}

/*******************************************************************************
 * DDS Priority Update
 ******************************************************************************/
static void update_priorities(dd_task_list *active_list) {
    dd_task_list *current;
    if (active_list == NULL) return;

    /* Earliest deadline -> HIGH priority, resume it */
    vTaskPrioritySet(active_list->task.t_handle, PRIORITY_USER_HIGH);
    vTaskResume(active_list->task.t_handle);

    /* All others -> LOW, suspended */
    current = active_list->next;
    while (current != NULL) {
        vTaskPrioritySet(current->task.t_handle, PRIORITY_USER_LOW);
        vTaskSuspend(current->task.t_handle);
        current = current->next;
    }
}

/*******************************************************************************
 * DDS F-Task
 ******************************************************************************/
static void dds_task_func(void *pvParameters) {
    dd_task_list *active_list    = NULL;
    uint32_t completed_count = 0;
    uint32_t overdue_count   = 0;
    dds_message msg;

    while (1) {
        if (xQueueReceive(dds_queue, &msg, portMAX_DELAY) == pdTRUE) {

            switch (msg.type) {

            case MSG_RELEASE_TASK: {
                dd_task new_task = msg.task_info;
                new_task.release_time = xTaskGetTickCount();

                safe_printf("Released Task %u (deadline %u) at %u\n",
                            (unsigned)new_task.task_id,
                            (unsigned)new_task.absolute_deadline,
                            (unsigned)new_task.release_time);

                /* Check for overdue tasks in active list */
                {
                    dd_task_list **pp = &active_list;
                    while (*pp != NULL) {
                        if (xTaskGetTickCount() > (*pp)->task.absolute_deadline) {
                            dd_task_list *overdue_node = *pp;
                            *pp = overdue_node->next;

                            safe_printf("Task %u OVERDUE!\n",
                                        (unsigned)overdue_node->task.task_id);

                            /* Explicitly suspend the FreeRTOS task so it stops executing! */
                            vTaskSuspend(overdue_node->task.t_handle);

                            /* Free node back to pool, increment counter */
                            node_free(overdue_node);
                            overdue_count++;
                            continue;
                        }
                        pp = &((*pp)->next);
                    }
                }

                /* Insert new task sorted by deadline */
                list_insert_sorted(&active_list, new_task);

                /* Update priorities for EDF */
                update_priorities(active_list);
                break;
            }

            case MSG_COMPLETE_TASK: {
                dd_task removed;
                if (list_remove_by_id(&active_list, msg.task_id, &removed)) {
                    removed.completion_time = xTaskGetTickCount();

                    if (removed.completion_time <= removed.absolute_deadline) {
                        completed_count++;
                        safe_printf("Task %u completed on time at %u\n",
                                    (unsigned)removed.task_id,
                                    (unsigned)removed.completion_time);
                    } else {
                        overdue_count++;
                        safe_printf("Task %u LATE at %u (deadline %u)\n",
                                    (unsigned)removed.task_id,
                                    (unsigned)removed.completion_time,
                                    (unsigned)removed.absolute_deadline);
                    }

                    update_priorities(active_list);
                }
                break;
            }

            case MSG_GET_ACTIVE_LIST: {
                dd_task_list *ptr = active_list;
                xQueueSend(response_queue, &ptr, portMAX_DELAY);
                break;
            }

            case MSG_GET_COMPLETED_LIST: {
                uint32_t count_val = completed_count;
                xQueueSend(response_queue, &count_val, portMAX_DELAY);
                break;
            }

            case MSG_GET_OVERDUE_LIST: {
                uint32_t count_val = overdue_count;
                xQueueSend(response_queue, &count_val, portMAX_DELAY);
                break;
            }

            default:
                break;
            }
        }
    }
}

/*******************************************************************************
 * User-Defined Tasks
 ******************************************************************************/

static void user_task_0(void *pv) {
    (void)pv;
    while (1) {
        uint32_t my_id = current_task_id[0];
        // safe_printf("  Task %u started (exec %u ms)\n",
        //            (unsigned)my_id, (unsigned)exec_times[0]);

        /* TRUE CPU ACTIVE BURN */
        uint32_t total_loops = exec_times[0] * LOOPS_PER_MS;
        for (volatile uint32_t i = 0; i < total_loops; i++) {
            /* Physically burn CPU cycles. If preempted, this loop pauses! */
        }

        // safe_printf("  Task %u finished\n", (unsigned)my_id);
        complete_dd_task(my_id);
        vTaskSuspend(NULL);
    }
}

static void user_task_1(void *pv) {
    (void)pv;
    while (1) {
        uint32_t my_id = current_task_id[1];
        // safe_printf("  Task %u started (exec %u ms)\n",
        //            (unsigned)my_id, (unsigned)exec_times[1]);

        /* TRUE CPU ACTIVE BURN */
        uint32_t total_loops = exec_times[1] * LOOPS_PER_MS;
        for (volatile uint32_t i = 0; i < total_loops; i++) {
            /* Physically burn CPU cycles. If preempted, this loop pauses! */
        }

        // safe_printf("  Task %u finished\n", (unsigned)my_id);
        complete_dd_task(my_id);
        vTaskSuspend(NULL);
    }
}

static void user_task_2(void *pv) {
    (void)pv;
    while (1) {
        uint32_t my_id = current_task_id[2];
        // safe_printf("  Task %u started (exec %u ms)\n",
        //            (unsigned)my_id, (unsigned)exec_times[2]);

        /* TRUE CPU ACTIVE BURN */
        uint32_t total_loops = exec_times[2] * LOOPS_PER_MS;
        for (volatile uint32_t i = 0; i < total_loops; i++) {
            /* Physically burn CPU cycles. If preempted, this loop pauses! */
        }

        // safe_printf("  Task %u finished\n", (unsigned)my_id);
        complete_dd_task(my_id);
        vTaskSuspend(NULL);
    }
}

static TaskFunction_t user_task_funcs[NUM_DD_TASKS] = {
    user_task_0, user_task_1, user_task_2
};

/*******************************************************************************
 * Timer Callbacks - Periodic Task Generators
 ******************************************************************************/

static void gen_timer_0_callback(TimerHandle_t xTimer) {
    (void)xTimer;
    uint32_t id = next_task_id++;
    current_task_id[0] = id;
    uint32_t deadline = xTaskGetTickCount() + task_periods[0];
//    safe_printf("[Gen] Task %u (type 0, deadline %u)\n",
//                (unsigned)id, (unsigned)deadline);
    release_dd_task(user_task_handles[0], PERIODIC, id, deadline);
}

static void gen_timer_1_callback(TimerHandle_t xTimer) {
    (void)xTimer;
    uint32_t id = next_task_id++;
    current_task_id[1] = id;
    uint32_t deadline = xTaskGetTickCount() + task_periods[1];
//    safe_printf("[Gen] Task %u (type 1, deadline %u)\n",
//                (unsigned)id, (unsigned)deadline);
    release_dd_task(user_task_handles[1], PERIODIC, id, deadline);
}

static void gen_timer_2_callback(TimerHandle_t xTimer) {
    (void)xTimer;
    uint32_t id = next_task_id++;
    current_task_id[2] = id;
    uint32_t deadline = xTaskGetTickCount() + task_periods[2];
//    safe_printf("[Gen] Task %u (type 2, deadline %u)\n",
//                (unsigned)id, (unsigned)deadline);
    release_dd_task(user_task_handles[2], PERIODIC, id, deadline);
}

static TimerCallbackFunction_t gen_callbacks[NUM_DD_TASKS] = {
    gen_timer_0_callback, gen_timer_1_callback, gen_timer_2_callback
};

/*******************************************************************************
 * Startup Task
 ******************************************************************************/
static void startup_task(void *pvParameters) {
    int i;
    (void)pvParameters;

    /* Release the first instance of each task type immediately at tick ~0 */
    for (i = 0; i < NUM_DD_TASKS; i++) {
        uint32_t id = next_task_id++;
        current_task_id[i] = id;
        uint32_t deadline = xTaskGetTickCount() + task_periods[i];
//        safe_printf("[Gen] Task %u (type %d, deadline %u)\n",
//                    (unsigned)id, i, (unsigned)deadline);
        release_dd_task(user_task_handles[i], PERIODIC, id, deadline);
    }

    /* Now start auto-reload timers for all subsequent periodic releases */
    for (i = 0; i < NUM_DD_TASKS; i++) {
        xTimerStart(gen_timers[i], portMAX_DELAY);
    }

    /* This task is no longer needed - suspend forever */
    vTaskSuspend(NULL);
}

/*******************************************************************************
 * Monitor Task
 ******************************************************************************/
static void monitor_task(void *pvParameters) {
    (void)pvParameters;
    vTaskDelay(pdMS_TO_TICKS(1000));

    while (1) {
        dd_task_list *active = get_active_dd_task_list();
        uint32_t n_active    = list_count(active);

        /* For completed and overdue, the DDS sends back counts directly */
        uint32_t n_completed = 0;
        uint32_t n_overdue   = 0;
        {
            dds_message msg;
            msg.type = MSG_GET_COMPLETED_LIST;
            xQueueSend(dds_queue, &msg, portMAX_DELAY);
            xQueueReceive(response_queue, &n_completed, portMAX_DELAY);
        }
        {
            dds_message msg;
            msg.type = MSG_GET_OVERDUE_LIST;
            xQueueSend(dds_queue, &msg, portMAX_DELAY);
            xQueueReceive(response_queue, &n_overdue, portMAX_DELAY);
        }

        safe_printf("\n=== Monitor (tick %u) ===\n",
                    (unsigned)xTaskGetTickCount());
        safe_printf("Active=%u  Completed=%u  Overdue=%u\n",
                    (unsigned)n_active, (unsigned)n_completed, (unsigned)n_overdue);

        {
            dd_task_list *curr = active;
            while (curr != NULL) {
                safe_printf("  Active: id=%u deadline=%u\n",
                            (unsigned)curr->task.task_id,
                            (unsigned)curr->task.absolute_deadline);
                curr = curr->next;
            }
        }
        safe_printf("========================\n\n");

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/*******************************************************************************
 * Main
 ******************************************************************************/
int main(void) {
    int i;
    char name[12];
    char tname[16];

    /* Init hardware */
    STM_EVAL_LEDInit(LED3);
    STM_EVAL_LEDInit(LED4);
    STM_EVAL_LEDInit(LED5);
    STM_EVAL_LEDInit(LED6);

    /* Init static node pool */
    node_pool_init();
    printf("Heap available: %u bytes\n", (unsigned)xPortGetFreeHeapSize());

    /* Create synchronization primitives */
    print_mutex = xSemaphoreCreateMutex();
    configASSERT(print_mutex);

    dds_queue = xQueueCreate(5, sizeof(dds_message));
    configASSERT(dds_queue);

    response_queue = xQueueCreate(1, sizeof(dd_task_list *));
    configASSERT(response_queue);

    /* Create DDS task - highest priority */
    xTaskCreate(dds_task_func, "DDS", configMINIMAL_STACK_SIZE * 2,
                NULL, PRIORITY_DDS, NULL);

    /* Create Monitor task - low priority */
    xTaskCreate(monitor_task, "Monitor", configMINIMAL_STACK_SIZE * 2,
                NULL, PRIORITY_MONITOR, NULL);

    /* Pre-create the 3 user F-Tasks (all start suspended) */
    for (i = 0; i < NUM_DD_TASKS; i++) {
        snprintf(name, sizeof(name), "User%d", i);
        xTaskCreate(user_task_funcs[i], name, configMINIMAL_STACK_SIZE * 2,
                    NULL, PRIORITY_USER_LOW, &user_task_handles[i]);
        vTaskSuspend(user_task_handles[i]);
    }

    /* Create software timers for periodic generation (not started yet) */
    for (i = 0; i < NUM_DD_TASKS; i++) {
        snprintf(tname, sizeof(tname), "GenT%d", i);
        gen_timers[i] = xTimerCreate(
            tname,
            pdMS_TO_TICKS(task_periods[i]),
            pdTRUE,
            NULL,
            gen_callbacks[i]
        );
        configASSERT(gen_timers[i]);
    }

    /* Create startup task - runs once to release first batch at tick 0,
     * then starts the timers for subsequent periods */
    xTaskCreate(startup_task, "Startup", configMINIMAL_STACK_SIZE * 2,
                NULL, PRIORITY_GENERATOR, NULL);

    /* Print configuration */
    printf("=== DDS Starting ===\n");
    printf("T1: exec=%u period=%u\n", (unsigned)TASK1_EXEC_TIME, (unsigned)TASK1_PERIOD);
    printf("T2: exec=%u period=%u\n", (unsigned)TASK2_EXEC_TIME, (unsigned)TASK2_PERIOD);
    printf("T3: exec=%u period=%u\n", (unsigned)TASK3_EXEC_TIME, (unsigned)TASK3_PERIOD);
    printf("====================\n\n");

    /* Start scheduler - does not return */
    vTaskStartScheduler();

    while (1);
}

/*******************************************************************************
 * FreeRTOS Hook Functions
 ******************************************************************************/
void vApplicationMallocFailedHook(void) {
    for (;;);
}

void vApplicationStackOverflowHook(xTaskHandle pxTask, signed char *pcTaskName) {
    (void)pcTaskName;
    (void)pxTask;
    for (;;);
}

void vApplicationIdleHook(void) {
    /* Nothing */
}

