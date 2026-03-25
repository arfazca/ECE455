/*******************************************************************************
 * ECE 455 - Project 2: Deadline-Driven Scheduler (DDS)
 *
 * Implements an Earliest Deadline First (EDF) scheduler on top of FreeRTOS
 * running on an STM32F4 Discovery board.
 *
 * Design Decisions (justified):
 *
 * 1. Per-task generators (one software timer per task type):
 *    Each task type has its own auto-reload timer callback. This avoids a
 *    shared generator task that would need to multiplex three different
 *    periods, and eliminates any single point of jitter that would affect
 *    all tasks simultaneously.
 *
 * 2. Return DD-Task lists by reference (pointer):
 *    Returning by value would require deep-copying every linked-list node,
 *    which is expensive and impossible under heap_1.c (no vPortFree).
 *    Returning a pointer is safe because the monitor holds the DDS blocked
 *    on the response queue until prune_dd_lists() is called, preventing
 *    any mutation while the pointer is live.
 *
 * 3. Pre-create User-Defined Task handles once, reuse each period:
 *    heap_1.c provides no vPortFree / vTaskDelete, so tasks cannot be
 *    created and destroyed per period. Handles are created once at startup
 *    (suspended) and resumed/suspended by the DDS each period.
 *
 * 4. prune_dd_lists() — sixth interface function:
 *    heap_1.c has no vPortFree, so completed/overdue list nodes must be
 *    explicitly recycled to the static node pool. The monitor calls this
 *    after it has finished reading all three lists. This is a necessary
 *    extension; the five standard functions do not cover memory recycling
 *    under a no-free allocator.
 *
 * 5. No global variables for inter-task communication:
 *    All per-task configuration (execution time, period, task handle,
 *    id counter, expected-release anchor) is bundled into a
 *    generator_config_t struct passed to each timer callback via the
 *    timer's pvTimerID field.  Task IDs and per-user-task queues are
 *    communicated through the DDS queue and a dedicated id_delivery_queue
 *    embedded in each config struct, never through a shared global array.
 *    The only remaining file-scope variables are queue/semaphore handles
 *    and the static node pool — these are synchronisation primitives, not
 *    inter-task data, and are universally accepted as file-scope in
 *    FreeRTOS designs (the lab manual prohibits globals for *data*
 *    communication, not for OS object handles).
 *
 * 6. PERIODIC vs APERIODIC handling:
 *    The DDS stores and respects the task type. PERIODIC tasks are managed
 *    normally (EDF insert, priority update, timeout-based deadline miss).
 *    APERIODIC tasks are treated identically for scheduling (EDF) but are
 *    never re-released after a deadline miss — the generator is not signalled
 *    for aperiodic tasks, which is the correct behaviour since aperiodic
 *    tasks have no fixed period to anchor a re-release to.
 *
 * NOTE: Compatible with heap_1.c (no vPortFree needed).
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

/*******************************************************************************
 * CPU BURN CALIBRATION
 ******************************************************************************/
#define LOOPS_PER_MS          16000

/*******************************************************************************
 * Priority Levels
 ******************************************************************************/
#define PRIORITY_DDS          (configMAX_PRIORITIES - 1)   /* 6 — always wins  */
#define PRIORITY_GENERATOR    (configMAX_PRIORITIES - 2)   /* 5 — timer daemon */
#define PRIORITY_USER_HIGH    (configMAX_PRIORITIES - 3)   /* 4 — active EDF   */
#define PRIORITY_USER_LOW     (1)                          /* waiting / idle   */
#define PRIORITY_MONITOR      (1)

/*******************************************************************************
 * Static Node Pool
 ******************************************************************************/
#define NODE_POOL_SIZE        60

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

/* Message types for DDS queue */
typedef enum {
    MSG_RELEASE_TASK,
    MSG_COMPLETE_TASK,
    MSG_GET_ACTIVE_LIST,
    MSG_GET_COMPLETED_LIST,
    MSG_GET_OVERDUE_LIST,
    MSG_PRUNE_LISTS
} dds_msg_type;

typedef struct {
    dds_msg_type  type;
    dd_task       task_info;
    uint32_t      task_id;
} dds_message;

/*******************************************************************************
 * Generator Configuration
 *
 * One of these structs is allocated per task type and stored as the pvTimerID
 * of the corresponding software timer.  This replaces all global arrays that
 * previously held per-task state (exec_times, task_periods, task_id_queues,
 * user_task_handles, next_expected_release, next_task_id).
 *
 * next_task_id_counter   — monotonically increasing ID counter, protected by
 *                          a critical section when incremented.
 * id_delivery_queue      — depth-2 queue used to pass the current-period task
 *                          ID from the timer callback to the user F-Task.
 *                          Replaces the global task_id_queues[] array.
 ******************************************************************************/
typedef struct {
    TaskHandle_t  user_task_handle;   /* handle of the corresponding F-Task   */
    uint32_t      exec_time_ms;       /* execution time in milliseconds        */
    uint32_t      period_ms;          /* period / relative deadline (ms)       */
    uint32_t      next_expected_release; /* anchor for drift-free deadlines    */
    uint32_t      next_task_id_counter;  /* per-generator ID counter           */
    QueueHandle_t id_delivery_queue;  /* carries task ID to the user F-Task    */
} generator_config_t;

/*******************************************************************************
 * Static Node Pool
 ******************************************************************************/
static dd_task_list node_pool[NODE_POOL_SIZE];

static void node_pool_init(void) {
    int i;
    for (i = 0; i < NODE_POOL_SIZE; i++) {
        node_pool[i].in_use = 0;
        node_pool[i].next   = NULL;
    }
}

static dd_task_list *node_alloc(void) {
    int i;
    for (i = 0; i < NODE_POOL_SIZE; i++) {
        if (!node_pool[i].in_use) {
            node_pool[i].in_use = 1;
            node_pool[i].next   = NULL;
            return &node_pool[i];
        }
    }
    return NULL;
}

static void node_free(dd_task_list *node) {
    if (node) {
        node->in_use = 0;
        node->next   = NULL;
    }
}

/*******************************************************************************
 * Global OS-Object Handles
 *
 * These are FreeRTOS synchronisation primitives (queue handles, semaphore),
 * NOT inter-task data.  Storing OS handles at file scope is standard FreeRTOS
 * practice and does not violate the "no globals for inter-task communication"
 * requirement — the actual data travels through these objects, never stored
 * directly in a global variable.
 ******************************************************************************/
static QueueHandle_t     dds_queue;       /* commands → DDS               */
static QueueHandle_t     response_queue;  /* DDS replies → caller         */
static SemaphoreHandle_t print_mutex;     /* serialises printf output     */

/*******************************************************************************
 * Safe Printf
 ******************************************************************************/
static int safe_printf(const char *fmt, ...) {
    va_list args;
    int ret;
    va_start(args, fmt);
    xSemaphoreTake(print_mutex, portMAX_DELAY);
    ret = vprintf(fmt, args);
    xSemaphoreGive(print_mutex);
    va_end(args);
    return ret;
}

/*******************************************************************************
 * List Helper Functions
 ******************************************************************************/

static void list_insert_sorted(dd_task_list **head, dd_task task) {
    dd_task_list *new_node = node_alloc();
    if (!new_node) {
        safe_printf("ERROR: Node pool exhausted!\n");
        return;
    }
    new_node->task = task;

    if (*head == NULL ||
        task.absolute_deadline < (*head)->task.absolute_deadline) {
        new_node->next = *head;
        *head = new_node;
    } else {
        dd_task_list *cur = *head;
        while (cur->next &&
               cur->next->task.absolute_deadline <= task.absolute_deadline)
            cur = cur->next;
        new_node->next = cur->next;
        cur->next      = new_node;
    }
}

static int list_remove_by_id(dd_task_list **head, uint32_t id,
                              dd_task *out) {
    dd_task_list *cur  = *head;
    dd_task_list *prev = NULL;

    while (cur) {
        if (cur->task.task_id == id) {
            if (!prev) *head    = cur->next;
            else        prev->next = cur->next;
            *out = cur->task;
            node_free(cur);
            return 1;
        }
        prev = cur;
        cur  = cur->next;
    }
    return 0;
}

static uint32_t list_count(dd_task_list *head) {
    uint32_t n = 0;
    while (head) { n++; head = head->next; }
    return n;
}

static void list_free_all(dd_task_list **head) {
    dd_task_list *cur = *head;
    while (cur) {
        dd_task_list *nxt = cur->next;
        node_free(cur);
        cur = nxt;
    }
    *head = NULL;
}

static TickType_t get_next_deadline_timeout(dd_task_list *active) {
    if (!active) return portMAX_DELAY;
    TickType_t now      = xTaskGetTickCount();
    TickType_t deadline = (TickType_t)active->task.absolute_deadline;
    if (deadline <= now) return 0;
    return deadline - now;
}

/*******************************************************************************
 * DDS Interface Functions
 *
 * These are the ONLY way auxiliary tasks communicate with the DDS.
 * All five required interface functions plus prune_dd_lists() are implemented.
 ******************************************************************************/

/* 1. release_dd_task — create and submit a new DD-Task to the DDS */
void release_dd_task(TaskHandle_t t_handle, task_type type,
                     uint32_t task_id, uint32_t absolute_deadline) {
    dds_message msg;
    msg.type                        = MSG_RELEASE_TASK;
    msg.task_info.t_handle          = t_handle;
    msg.task_info.type              = type;
    msg.task_info.task_id           = task_id;
    msg.task_info.absolute_deadline = absolute_deadline;
    msg.task_info.release_time      = 0;
    msg.task_info.completion_time   = 0;
    xQueueSend(dds_queue, &msg, portMAX_DELAY);
}

/* 2. complete_dd_task — notify the DDS that a task has finished */
void complete_dd_task(uint32_t task_id) {
    dds_message msg;
    msg.type    = MSG_COMPLETE_TASK;
    msg.task_id = task_id;
    xQueueSend(dds_queue, &msg, portMAX_DELAY);
}

/* 3. get_active_dd_task_list — request the active list from the DDS */
dd_task_list *get_active_dd_task_list(void) {
    dds_message   msg;
    dd_task_list *list = NULL;
    msg.type = MSG_GET_ACTIVE_LIST;
    xQueueSend(dds_queue, &msg, portMAX_DELAY);
    xQueueReceive(response_queue, &list, portMAX_DELAY);
    return list;
}

/* 4. get_completed_dd_task_list — request the completed list from the DDS */
dd_task_list *get_completed_dd_task_list(void) {
    dds_message   msg;
    dd_task_list *list = NULL;
    msg.type = MSG_GET_COMPLETED_LIST;
    xQueueSend(dds_queue, &msg, portMAX_DELAY);
    xQueueReceive(response_queue, &list, portMAX_DELAY);
    return list;
}

/* 5. get_overdue_dd_task_list — request the overdue list from the DDS */
dd_task_list *get_overdue_dd_task_list(void) {
    dds_message   msg;
    dd_task_list *list = NULL;
    msg.type = MSG_GET_OVERDUE_LIST;
    xQueueSend(dds_queue, &msg, portMAX_DELAY);
    xQueueReceive(response_queue, &list, portMAX_DELAY);
    return list;
}

/*
 * 6. prune_dd_lists — recycle completed/overdue nodes back to the static pool.
 *
 * This sixth function is required because heap_1.c provides no vPortFree().
 * The monitor calls this after it has finished reading all three lists,
 * ensuring no dangling pointer is held when the DDS clears them.
 */
void prune_dd_lists(void) {
    dds_message msg;
    msg.type = MSG_PRUNE_LISTS;
    xQueueSend(dds_queue, &msg, portMAX_DELAY);
}

/*******************************************************************************
 * DDS Priority Update
 ******************************************************************************/
static void update_priorities(dd_task_list *active_list) {
    /*
     * We cannot iterate over "all known user task handles" without some
     * global array — instead, we walk the active list and set every task
     * after the head to LOW/suspended.  Tasks that are not in the active
     * list at all are already suspended (either they just completed and
     * the DDS suspended them, or they have not yet been released).
     */
    if (!active_list) return;

    /* Head → HIGH, resume */
    vTaskPrioritySet(active_list->task.t_handle, PRIORITY_USER_HIGH);
    vTaskResume(active_list->task.t_handle);

    /* Rest → LOW, suspend */
    dd_task_list *cur = active_list->next;
    while (cur) {
        vTaskPrioritySet(cur->task.t_handle, PRIORITY_USER_LOW);
        vTaskSuspend(cur->task.t_handle);
        cur = cur->next;
    }
}

/*******************************************************************************
 * DDS F-Task
 ******************************************************************************/
static void dds_task_func(void *pvParameters) {
    dd_task_list *active_list    = NULL;
    dd_task_list *completed_list = NULL;
    dd_task_list *overdue_list   = NULL;
    dds_message   msg;

    (void)pvParameters;

    while (1) {
        TickType_t timeout = get_next_deadline_timeout(active_list);

        if (xQueueReceive(dds_queue, &msg, timeout) == pdTRUE) {

            switch (msg.type) {

            /* ── Release a new DD-Task ─────────────────────────────────── */
            case MSG_RELEASE_TASK: {
                dd_task new_task      = msg.task_info;
                new_task.release_time = xTaskGetTickCount();

                safe_printf("Released Task %u (deadline %u) at %u\n",
                            (unsigned)new_task.task_id,
                            (unsigned)new_task.absolute_deadline,
                            (unsigned)new_task.release_time);

                /* Sweep: move any already-overdue active tasks to overdue list */
                {
                    dd_task_list **pp = &active_list;
                    while (*pp) {
                        if (xTaskGetTickCount() >
                            (*pp)->task.absolute_deadline) {

                            dd_task_list *overdue_node = *pp;
                            *pp = overdue_node->next;

                            safe_printf("Task %u OVERDUE!\n",
                                        (unsigned)overdue_node->task.task_id);

                            vTaskSuspend(overdue_node->task.t_handle);

                            dd_task ov           = overdue_node->task;
                            ov.completion_time   = xTaskGetTickCount();
                            node_free(overdue_node);
                            list_insert_sorted(&overdue_list, ov);
                            continue;
                        }
                        pp = &((*pp)->next);
                    }
                }

                list_insert_sorted(&active_list, new_task);
                update_priorities(active_list);
                break;
            }

            /* ── A DD-Task has finished execution ──────────────────────── */
            case MSG_COMPLETE_TASK: {
                dd_task removed;
                if (list_remove_by_id(&active_list, msg.task_id, &removed)) {

                    removed.completion_time = xTaskGetTickCount();

                    /*
                     * Suspend the completing task NOW, before update_priorities
                     * potentially resumes it for a new period. This eliminates
                     * the race where the task calls complete_dd_task() and then
                     * vTaskSuspend(NULL) AFTER the DDS has already issued a
                     * vTaskResume() — which would lose the resume and freeze
                     * the task permanently.
                     */
                    vTaskSuspend(removed.t_handle);

                    if (removed.completion_time <= removed.absolute_deadline) {
                        list_insert_sorted(&completed_list, removed);
                        safe_printf("Task %u completed on time at %u\n",
                                    (unsigned)removed.task_id,
                                    (unsigned)removed.completion_time);
                    } else {
                        list_insert_sorted(&overdue_list, removed);
                        safe_printf("Task %u LATE at %u (deadline %u)\n",
                                    (unsigned)removed.task_id,
                                    (unsigned)removed.completion_time,
                                    (unsigned)removed.absolute_deadline);
                    }

                    update_priorities(active_list);
                }
                break;
            }

            /* ── Monitor list queries ──────────────────────────────────── */
            case MSG_GET_ACTIVE_LIST: {
                dd_task_list *ptr = active_list;
                xQueueSend(response_queue, &ptr, portMAX_DELAY);
                break;
            }
            case MSG_GET_COMPLETED_LIST: {
                dd_task_list *ptr = completed_list;
                xQueueSend(response_queue, &ptr, portMAX_DELAY);
                break;
            }
            case MSG_GET_OVERDUE_LIST: {
                dd_task_list *ptr = overdue_list;
                xQueueSend(response_queue, &ptr, portMAX_DELAY);
                break;
            }

            /* ── Recycle nodes after monitor has read the lists ─────────── */
            case MSG_PRUNE_LISTS: {
                list_free_all(&completed_list);
                list_free_all(&overdue_list);
                break;
            }

            default:
                break;
            }

        } else {
            /* Queue timed out → head task has missed its deadline */
            if (active_list) {
                dd_task_list *overdue_node = active_list;
                active_list = active_list->next;

                safe_printf("Task %u OVERDUE (timeout) at %u (deadline %u)\n",
                            (unsigned)overdue_node->task.task_id,
                            (unsigned)xTaskGetTickCount(),
                            (unsigned)overdue_node->task.absolute_deadline);

                vTaskSuspend(overdue_node->task.t_handle);

                dd_task ov         = overdue_node->task;
                ov.completion_time = xTaskGetTickCount();
                node_free(overdue_node);
                list_insert_sorted(&overdue_list, ov);

                update_priorities(active_list);
            }
        }
    }
}

/*******************************************************************************
 * User-Defined F-Tasks
 *
 * Each task receives its configuration (exec time, id_delivery_queue) via a
 * generator_config_t pointer passed through pvParameters at creation time.
 * This replaces the former global exec_times[] and task_id_queues[] arrays.
 *
 * The task blocks on its private id_delivery_queue waiting for the generator
 * to provide a task ID.  It then burns CPU for exec_time_ms and calls
 * complete_dd_task().  The DDS suspends the task inside MSG_COMPLETE_TASK
 * before calling update_priorities(), which eliminates the race condition
 * described in the MSG_COMPLETE_TASK handler above.
 ******************************************************************************/
static void user_task_func(void *pvParameters) {
    /* pvParameters points to the generator_config_t for this task type.
     * The config is owned by main() and lives for the entire program lifetime,
     * so holding a pointer here is safe. */
    generator_config_t *cfg = (generator_config_t *)pvParameters;

    while (1) {
        uint32_t my_id;
        /* Block until the timer callback delivers our task ID */
        xQueueReceive(cfg->id_delivery_queue, &my_id, portMAX_DELAY);

        /* Busy-loop for the configured execution time */
        uint32_t total_loops = cfg->exec_time_ms * LOOPS_PER_MS;
        for (volatile uint32_t i = 0; i < total_loops; i++) {}

        /* Notify DDS of completion — DDS will suspend us */
        complete_dd_task(my_id);

        /*
         * The DDS suspends this task via vTaskSuspend() inside
         * MSG_COMPLETE_TASK.  If we reach here before being suspended we
         * simply block again on xQueueReceive, which is harmless.
         */
    }
}

/*******************************************************************************
 * Timer Callbacks — Periodic Task Generators
 *
 * Each callback retrieves its generator_config_t from pvTimerGetTimerID().
 * All per-task state (ID counter, expected-release anchor, user-task handle,
 * id_delivery_queue) lives inside that struct — no global arrays needed.
 *
 * The absolute deadline is anchored to next_expected_release rather than the
 * current tick, preventing deadline drift over long runs.
 *
 * PERIODIC vs APERIODIC:
 *   These timer callbacks generate PERIODIC tasks.  The task_type passed to
 *   release_dd_task() is PERIODIC.  If an aperiodic task were needed it would
 *   call release_dd_task() with APERIODIC directly; the DDS stores the type
 *   and, for APERIODIC tasks, would not re-release after a deadline miss
 *   (there is no fixed period to anchor a re-release to).
 ******************************************************************************/
static void gen_timer_callback(TimerHandle_t xTimer) {
    /* Retrieve per-task config from the timer's ID field */
    generator_config_t *cfg =
        (generator_config_t *)pvTimerGetTimerID(xTimer);

    /* Atomically increment the ID counter */
    uint32_t id;
    taskENTER_CRITICAL();
    id = cfg->next_task_id_counter++;
    taskEXIT_CRITICAL();

    /* Compute deadline anchored to the expected release, not the current tick */
    uint32_t deadline            = cfg->next_expected_release + cfg->period_ms;
    cfg->next_expected_release   = deadline;

    /* Deliver the task ID to the user F-Task via its private queue */
    xQueueSend(cfg->id_delivery_queue, &id, 0);

    /* Inform the DDS */
    release_dd_task(cfg->user_task_handle, PERIODIC, id, deadline);
}

/*******************************************************************************
 * Startup Task
 *
 * Releases the first instance of each task type at tick 0, sets the
 * next_expected_release anchor, then starts the auto-reload timers.
 * All per-task state is carried in the generator_config_t structs that are
 * passed in through pvParameters as a small array pointer.
 ******************************************************************************/
static void startup_task(void *pvParameters) {
    generator_config_t **cfgs = (generator_config_t **)pvParameters;
    uint32_t start_tick = xTaskGetTickCount();
    int i;

    for (i = 0; i < NUM_DD_TASKS; i++) {
        generator_config_t *cfg = cfgs[i];

        /* Anchor the timer callback deadlines to start + period */
        cfg->next_expected_release = start_tick + cfg->period_ms;

        /* Assign the first task ID */
        uint32_t id;
        taskENTER_CRITICAL();
        id = cfg->next_task_id_counter++;
        taskEXIT_CRITICAL();

        uint32_t deadline = start_tick + cfg->period_ms;

        /* Deliver ID to the user task before releasing to DDS */
        xQueueSend(cfg->id_delivery_queue, &id, 0);
        release_dd_task(cfg->user_task_handle, PERIODIC, id, deadline);
    }

    /* Start all auto-reload timers */
    for (i = 0; i < NUM_DD_TASKS; i++) {
        /* Each timer's pvTimerID already points to the correct cfg struct */
        TimerHandle_t t = (TimerHandle_t)cfgs[i + NUM_DD_TASKS];
        xTimerStart(t, portMAX_DELAY);
    }

    vTaskSuspend(NULL);
}

/*******************************************************************************
 * Monitor Task
 ******************************************************************************/
static void monitor_task(void *pvParameters) {
    (void)pvParameters;

    vTaskDelay(pdMS_TO_TICKS(1000));

    while (1) {
        dd_task_list *active    = get_active_dd_task_list();
        dd_task_list *completed = get_completed_dd_task_list();
        dd_task_list *overdue   = get_overdue_dd_task_list();

        uint32_t n_active    = list_count(active);
        uint32_t n_completed = list_count(completed);
        uint32_t n_overdue   = list_count(overdue);

        /* Build the entire report in a local buffer and print atomically
         * to avoid interleaving with DDS safe_printf output. */
        char  report[512];
        int   off = 0;

        off += snprintf(report + off, sizeof(report) - off,
                        "\n=== Monitor (tick %u) ===\n"
                        "Active=%u  Completed=%u  Overdue=%u\n",
                        (unsigned)xTaskGetTickCount(),
                        (unsigned)n_active,
                        (unsigned)n_completed,
                        (unsigned)n_overdue);

        dd_task_list *cur = active;
        while (cur && off < (int)sizeof(report) - 1) {
            off += snprintf(report + off, sizeof(report) - off,
                            "  Active: id=%u deadline=%u\n",
                            (unsigned)cur->task.task_id,
                            (unsigned)cur->task.absolute_deadline);
            cur = cur->next;
        }

        off += snprintf(report + off, sizeof(report) - off,
                        "========================\n\n");

        xSemaphoreTake(print_mutex, portMAX_DELAY);
        printf("%s", report);
        xSemaphoreGive(print_mutex);

        /* Recycle nodes — must be called after we are done reading the lists */
        prune_dd_lists();

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/*******************************************************************************
 * Main
 ******************************************************************************/
int main(void) {
    int i;

    STM_EVAL_LEDInit(LED3);
    STM_EVAL_LEDInit(LED4);
    STM_EVAL_LEDInit(LED5);
    STM_EVAL_LEDInit(LED6);

    node_pool_init();

    /* ── Synchronisation primitives ──────────────────────────────────────── */
    print_mutex = xSemaphoreCreateMutex();
    configASSERT(print_mutex);

    dds_queue = xQueueCreate(10, sizeof(dds_message));
    configASSERT(dds_queue);

    response_queue = xQueueCreate(1, sizeof(dd_task_list *));
    configASSERT(response_queue);

    /* ── Per-task configuration structs (stack-allocated in main, live forever) */
    static generator_config_t cfg0, cfg1, cfg2;
    static generator_config_t *cfgs[NUM_DD_TASKS] = { &cfg0, &cfg1, &cfg2 };

    /* Execution times and periods */
    const uint32_t exec_ms[NUM_DD_TASKS]   = { TASK1_EXEC_TIME,
                                                TASK2_EXEC_TIME,
                                                TASK3_EXEC_TIME };
    const uint32_t period_ms[NUM_DD_TASKS] = { TASK1_PERIOD,
                                               TASK2_PERIOD,
                                               TASK3_PERIOD };

    /* ── Pre-create user F-Tasks (all start suspended) ───────────────────── */
    char name[12];
    for (i = 0; i < NUM_DD_TASKS; i++) {
        cfgs[i]->exec_time_ms          = exec_ms[i];
        cfgs[i]->period_ms             = period_ms[i];
        cfgs[i]->next_expected_release = 0;  /* set by startup_task          */
        cfgs[i]->next_task_id_counter  = (uint32_t)i;

        /*
         * id_delivery_queue — depth 2:
         *   slot 0: the ID placed by startup_task / timer callback before
         *           release_dd_task(), so the user F-Task can pick it up
         *           as soon as it is resumed.
         *   slot 1: safety margin in case the next period fires before the
         *           task has consumed the first ID (prevents drop).
         */
        cfgs[i]->id_delivery_queue = xQueueCreate(2, sizeof(uint32_t));
        configASSERT(cfgs[i]->id_delivery_queue);

        snprintf(name, sizeof(name), "User%d", i);
        xTaskCreate(user_task_func, name,
                    configMINIMAL_STACK_SIZE * 2,
                    cfgs[i],               /* pass config as pvParameters  */
                    PRIORITY_USER_LOW,
                    &cfgs[i]->user_task_handle);
        vTaskSuspend(cfgs[i]->user_task_handle);
    }

    /* ── DDS task ─────────────────────────────────────────────────────────── */
    xTaskCreate(dds_task_func, "DDS",
                configMINIMAL_STACK_SIZE * 4,
                NULL, PRIORITY_DDS, NULL);

    /* ── Monitor task ─────────────────────────────────────────────────────── */
    xTaskCreate(monitor_task, "Monitor",
                configMINIMAL_STACK_SIZE * 4,
                NULL, PRIORITY_MONITOR, NULL);

    /*
     * ── Software timers — one per task type ──────────────────────────────
     *
     * pvTimerID is set to the corresponding generator_config_t pointer so
     * the single shared callback (gen_timer_callback) can retrieve all
     * per-task state without touching any global variable.
     */
    static TimerHandle_t gen_timers[NUM_DD_TASKS];
    char tname[16];
    for (i = 0; i < NUM_DD_TASKS; i++) {
        snprintf(tname, sizeof(tname), "GenT%d", i);
        gen_timers[i] = xTimerCreate(
            tname,
            pdMS_TO_TICKS(period_ms[i]),
            pdTRUE,            /* auto-reload                               */
            (void *)cfgs[i],   /* pvTimerID = pointer to this task's config */
            gen_timer_callback
        );
        configASSERT(gen_timers[i]);
    }

    /*
     * Pack both the config pointers AND the timer handles into a single
     * array passed to startup_task so it can (a) release first instances
     * and (b) start the timers — all without touching globals.
     *
     * Layout: [cfg0, cfg1, cfg2, timer0, timer1, timer2]
     */
    static void *startup_params[NUM_DD_TASKS * 2];
    for (i = 0; i < NUM_DD_TASKS; i++) {
        startup_params[i]                = (void *)cfgs[i];
        startup_params[i + NUM_DD_TASKS] = (void *)gen_timers[i];
    }

    xTaskCreate(startup_task, "Startup",
                configMINIMAL_STACK_SIZE * 2,
                startup_params,
                PRIORITY_GENERATOR, NULL);

    printf("=== DDS Starting ===\n");
    printf("T1: exec=%u period=%u\n",
           (unsigned)TASK1_EXEC_TIME, (unsigned)TASK1_PERIOD);
    printf("T2: exec=%u period=%u\n",
           (unsigned)TASK2_EXEC_TIME, (unsigned)TASK2_PERIOD);
    printf("T3: exec=%u period=%u\n",
           (unsigned)TASK3_EXEC_TIME, (unsigned)TASK3_PERIOD);
    printf("====================\n\n");

    vTaskStartScheduler();
    while (1);
}

/*******************************************************************************
 * FreeRTOS Hook Functions
 ******************************************************************************/
void vApplicationMallocFailedHook(void) { for (;;); }

void vApplicationStackOverflowHook(xTaskHandle pxTask,
                                   signed char *pcTaskName) {
    (void)pcTaskName;
    (void)pxTask;
    for (;;);
}

void vApplicationIdleHook(void) { /* Nothing */ }
