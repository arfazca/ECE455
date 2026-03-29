#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include "stm32f4_discovery.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "timers.h"

#define TEST_BENCH 1
#if TEST_BENCH == 1
#define T1_EXEC_MS		95
#define T1_PERIOD_MS	500
#define T2_EXEC_MS		150
#define T2_PERIOD_MS	500
#define T3_EXEC_MS		250
#define T3_PERIOD_MS	750

#elif TEST_BENCH == 2
#define T1_EXEC_MS		95
#define T1_PERIOD_MS	250
#define T2_EXEC_MS		150
#define T2_PERIOD_MS	500
#define T3_EXEC_MS		250
#define T3_PERIOD_MS	750

#elif TEST_BENCH == 3
#define T1_EXEC_MS		100
#define T1_PERIOD_MS	500
#define T2_EXEC_MS		200
#define T2_PERIOD_MS	500
#define T3_EXEC_MS		200
#define T3_PERIOD_MS	500

#else
#error "TEST_BENCH must be 1, 2, or 3"
#endif

#define PRIORITY_DDS		5
#define PRIORITY_MONITOR	4
#define PRIORITY_GENERATOR	3
#define PRIORITY_USER_HIGH	2
#define PRIORITY_USER_LOW	1

#define TICKS_TO_MS(t)		((uint32_t)((t) * 1000u / configTICK_RATE_HZ))
#define GENERATOR_TOKEN		((uint32_t)1u)

typedef enum { PERIODIC, APERIODIC } task_type;

typedef struct {
	TaskHandle_t	t_handle;
	task_type		type;
	uint32_t		task_id;
	uint32_t		instance_id;
	uint32_t		release_time;
	uint32_t		absolute_deadline;
	uint32_t		completion_time;
} dd_task;

typedef struct dd_task_node {
	dd_task					task;
	struct dd_task_node		*next;
} dd_task_list;

#define MSG_RELEASE			1u
#define MSG_COMPLETE		2u
#define MSG_GET_ACTIVE		3u
#define MSG_GET_COMPLETED	4u
#define MSG_GET_OVERDUE		5u

typedef struct {
	uint32_t		msg_type;
	dd_task			task;
	QueueHandle_t	reply_queue;
} dds_message;

typedef struct { dd_task_list *list; } dds_reply;

static QueueHandle_t		dds_queue;
static QueueHandle_t		generator_queue;
static QueueHandle_t		task1_release_queue;
static QueueHandle_t		task2_release_queue;
static QueueHandle_t		task3_release_queue;
static TimerHandle_t		release_timer;
static SemaphoreHandle_t	print_mutex;

static dd_task_list	*active_list	= NULL;
static dd_task_list	*completed_list	= NULL;
static dd_task_list	*overdue_list	= NULL;
static uint32_t next_instance_id = 1;
static TaskHandle_t task1_handle = NULL;
static TaskHandle_t task2_handle = NULL;
static TaskHandle_t task3_handle = NULL;

static TickType_t t1_next_release_tick	= 0;
static TickType_t t2_next_release_tick	= 0;
static TickType_t t3_next_release_tick	= 0;
static TickType_t t1_next_deadline_tick	= 0;
static TickType_t t2_next_deadline_tick	= 0;
static TickType_t t3_next_deadline_tick	= 0;

void DD_Scheduler_Task(void *pv);
void DD_Generator_Task(void *pv);
void Monitor_Task(void *pv);
void User_Defined_Task_1(void *pv);
void User_Defined_Task_2(void *pv);
void User_Defined_Task_3(void *pv);

void					insert_node(dd_task_list **head, dd_task task);
dd_task_list			*remove_node(dd_task_list **head, uint32_t instance_id);
uint32_t				get_list_size(dd_task_list *head);
TickType_t				get_next_timeout(void);
void					adjust_priorities(void);
void					handle_release(dds_message *msg);
void					handle_complete(dds_message *msg);
void					handle_deadline_miss(void);

uint32_t				release_dd_task(TaskHandle_t h, task_type t,
							uint32_t id, uint32_t relative_deadline);
void					complete_dd_task(uint32_t instance_id);
dd_task_list			*get_active_dd_task_list(void);
dd_task_list			*get_completed_dd_task_list(void);
dd_task_list			*get_overdue_dd_task_list(void);

static void				dwt_init(void);
static void				busy_wait_ms(uint32_t ms);
static QueueHandle_t	get_release_queue(uint32_t id);
static TaskHandle_t		get_task_handle_for(uint32_t id);
static TickType_t		get_period_ticks(uint32_t id);
static TickType_t		get_next_release_min(void);
static void				arm_release_timer(void);
static void				release_due_tasks(void);
static void				release_one_task(uint32_t id, task_type type,
							TickType_t rel, TickType_t dl);
static void				release_timer_cb(TimerHandle_t xTimer);
static void				sem_printf(const char *fmt, ...);

// prints stuff but uses mutex so threads dont mess up output  TBH just safer printf
// kinda wraps vprintf and locks before printing then unlocks after
static void sem_printf(const char *fmt, ...)
{
	va_list args;
	if (print_mutex != NULL)
		xSemaphoreTake(print_mutex, portMAX_DELAY);
	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
	if (print_mutex != NULL)
		xSemaphoreGive(print_mutex);
}

#define REG_DEMCR			(*((volatile uint32_t *)0xE000EDFCu))
#define REG_DWT_CTRL		(*((volatile uint32_t *)0xE0001000u))
#define REG_DWT_CYCCNT		(*((volatile uint32_t *)0xE0001004u))
#define DEMCR_TRCENA_BIT	(1UL << 24)
#define DWT_CYCCNTENA_BIT	(1UL << 0)

// init the cycle counter thingy (DWT) so we can measure time more accurately
// needed for busy wait otherwise timing would be off
static void dwt_init(void)
{
	REG_DEMCR |= DEMCR_TRCENA_BIT;
	REG_DWT_CYCCNT = 0;
	REG_DWT_CTRL |= DWT_CYCCNTENA_BIT;
}

// just wastes time for given ms using cpu cycles (not ideal but ok here)
// uses DWT counter so kinda precise  but still busy waiting
static void busy_wait_ms(uint32_t ms)
{
	uint32_t cycles_needed = ms * (SystemCoreClock / 1000UL);
	uint32_t cycles_done = 0;
	uint32_t prev_cyc = REG_DWT_CYCCNT;
	TickType_t prev_tick = xTaskGetTickCount();

	while (cycles_done < cycles_needed) {
		uint32_t now_cyc = REG_DWT_CYCCNT;
		TickType_t now_tick = xTaskGetTickCount();
		if (now_tick == prev_tick)
			cycles_done += (now_cyc - prev_cyc);
		prev_cyc = now_cyc;
		prev_tick = now_tick;
	}
}

// inserts task into linked list sorted by deadline (EDF style)
// earlier deadline = higher priority so goes closer to head
void insert_node(dd_task_list **head, dd_task task)
{
	dd_task_list *new_node;
	dd_task_list *curr;
	new_node = (dd_task_list *)pvPortMalloc(sizeof(dd_task_list));
	if (new_node == NULL) return;

	new_node->task = task;
	new_node->next = NULL;

	if (*head == NULL) { *head = new_node; return; }
	if (task.absolute_deadline < (*head)->task.absolute_deadline) {
		new_node->next = *head;
		*head = new_node;
		return;
	}

	curr = *head;
	while (curr->next != NULL &&
		   curr->next->task.absolute_deadline <= task.absolute_deadline)
		curr = curr->next;

	new_node->next = curr->next;
	curr->next = new_node;
}

// removes a task from list using instance id
// returns the node removed so we can reuse it or move it to other list
dd_task_list *remove_node(dd_task_list **head, uint32_t instance_id)
{
	dd_task_list *prev, *curr;
	if (*head == NULL) return NULL;

	if ((*head)->task.instance_id == instance_id) {
		dd_task_list *r = *head;
		*head = (*head)->next;
		r->next = NULL;
		return r;
	}

	prev = *head;
	curr = (*head)->next;
	while (curr != NULL) {
		if (curr->task.instance_id == instance_id) {
			prev->next = curr->next;
			curr->next = NULL;
			return curr;
		}
		prev = curr;
		curr = curr->next;
	}
	return NULL;
}

// counts how many nodes in a list  pretty straightforward
// used for monitor stats mostly
uint32_t get_list_size(dd_task_list *head)
{
	uint32_t n = 0;
	while (head != NULL) { n++; head = head->next; }
	return n;
}

// figures out how long scheduler should wait before next deadline miss
// if no tasks active then just wait forever basically
TickType_t get_next_timeout(void)
{
	TickType_t now, dl;
	if (active_list == NULL) return portMAX_DELAY;
	now = xTaskGetTickCount();
	dl = (TickType_t)active_list->task.absolute_deadline;
	if (dl <= now) return 0;
	return dl - now;
}

// updates task priorities based on EDF
// first task (earliest deadline) gets high prio others low
void adjust_priorities(void)
{
	dd_task_list *curr = active_list;
	while (curr != NULL) {
		vTaskPrioritySet(curr->task.t_handle,
						 (curr == active_list) ? PRIORITY_USER_HIGH
											   : PRIORITY_USER_LOW);
		curr = curr->next;
	}
}

// handles when a task is released into system
// sets release time + absolute deadline and adds to active list
void handle_release(dds_message *msg)
{
	TickType_t now = xTaskGetTickCount();
	msg->task.release_time = (uint32_t)now;
	msg->task.absolute_deadline = (uint32_t)(now + (TickType_t)msg->task.absolute_deadline);
	insert_node(&active_list, msg->task);
	adjust_priorities();
	sem_printf("T%u R:%ums\n",
		   (unsigned int)msg->task.task_id,
		   (unsigned int)TICKS_TO_MS(msg->task.release_time));
}

// called when task finishes execution
// removes from active list and pushes into completed list
void handle_complete(dds_message *msg)
{
	dd_task_list *node = remove_node(&active_list, msg->task.instance_id);
	if (node != NULL) {
		node->task.completion_time = xTaskGetTickCount();
		vTaskPrioritySet(node->task.t_handle, PRIORITY_USER_LOW);
		node->next = completed_list;
		completed_list = node;
		adjust_priorities();
		sem_printf("T%u C:%ums\n",
			   (unsigned int)node->task.task_id,
			   (unsigned int)TICKS_TO_MS(node->task.completion_time));
	} else {
		adjust_priorities();
	}
}

// if a task misses deadline we move it to overdue list
// also lower its priority so it doesnt block others
void handle_deadline_miss(void)
{
	dd_task_list *missed;
	if (active_list == NULL) return;
	missed = active_list;
	active_list = active_list->next;
	missed->next = NULL;
	vTaskPrioritySet(missed->task.t_handle, PRIORITY_USER_LOW);
	missed->next = overdue_list;
	overdue_list = missed;
	adjust_priorities();
	sem_printf("T%u M:%ums\n",
		   (unsigned int)missed->task.task_id,
		   (unsigned int)TICKS_TO_MS(xTaskGetTickCount()));
}

// main scheduler task (core of DDS)
// waits for messages like release/complete and handles deadlines
void DD_Scheduler_Task(void *pv)
{
	dds_message msg;
	(void)pv;

	for (;;) {
		TickType_t timeout = get_next_timeout();
		BaseType_t received = xQueueReceive(dds_queue, &msg, timeout);

		if (received == pdTRUE) {
			switch (msg.msg_type) {
				case MSG_RELEASE:	handle_release(&msg);	break;
				case MSG_COMPLETE:	handle_complete(&msg);	break;
				case MSG_GET_ACTIVE:
				{
					dds_reply r = { active_list };
					xQueueSend(msg.reply_queue, &r, portMAX_DELAY);
					break;
				}
				case MSG_GET_COMPLETED:
				{
					dds_reply r = { completed_list };
					xQueueSend(msg.reply_queue, &r, portMAX_DELAY);
					break;
				}
				case MSG_GET_OVERDUE:
				{
					dds_reply r = { overdue_list };
					xQueueSend(msg.reply_queue, &r, portMAX_DELAY);
					break;
				}
				default: break;
			}
		} else { handle_deadline_miss(); }
	}
}

// sends a release msg to scheduler with task info
// also assigns unique instance id
uint32_t release_dd_task(TaskHandle_t h, task_type t, uint32_t id,
					 uint32_t relative_deadline)
{
	dds_message msg;
	uint32_t iid = next_instance_id++;
	msg.msg_type = MSG_RELEASE;
	msg.task.t_handle = h;
	msg.task.type = t;
	msg.task.task_id = id;
	msg.task.instance_id = iid;
	msg.task.release_time = 0;
	msg.task.absolute_deadline = relative_deadline;
	msg.task.completion_time = 0;
	msg.reply_queue = NULL;
	xQueueSend(dds_queue, &msg, portMAX_DELAY);
	return iid;
}

// sends completion msg to scheduler when task is done
// pretty much signals DDS that work finished
void complete_dd_task(uint32_t instance_id)
{
	dds_message msg;
	msg.msg_type = MSG_COMPLETE;
	msg.task.instance_id = instance_id;
	msg.reply_queue = NULL;
	xQueueSend(dds_queue, &msg, portMAX_DELAY);
}

// asks scheduler for active tasks list
// uses queue to get reply (kind of request-response pattern)
dd_task_list *get_active_dd_task_list(void)
{
	QueueHandle_t rq = xQueueCreate(1, sizeof(dds_reply));
	dds_message msg; dds_reply reply;
	msg.msg_type = MSG_GET_ACTIVE; msg.reply_queue = rq;
	xQueueSend(dds_queue, &msg, portMAX_DELAY);
	xQueueReceive(rq, &reply, portMAX_DELAY);
	vQueueDelete(rq);
	return reply.list;
}

// same idea but for completed tasks
// used by monitor to print stats
dd_task_list *get_completed_dd_task_list(void)
{
	QueueHandle_t rq = xQueueCreate(1, sizeof(dds_reply));
	dds_message msg; dds_reply reply;
	msg.msg_type = MSG_GET_COMPLETED; msg.reply_queue = rq;
	xQueueSend(dds_queue, &msg, portMAX_DELAY);
	xQueueReceive(rq, &reply, portMAX_DELAY);
	vQueueDelete(rq);
	return reply.list;
}

// gets tasks that missed deadlines
// again just querying scheduler
dd_task_list *get_overdue_dd_task_list(void)
{
	QueueHandle_t rq = xQueueCreate(1, sizeof(dds_reply));
	dds_message msg; dds_reply reply;
	msg.msg_type = MSG_GET_OVERDUE; msg.reply_queue = rq;
	xQueueSend(dds_queue, &msg, portMAX_DELAY);
	xQueueReceive(rq, &reply, portMAX_DELAY);
	vQueueDelete(rq);
	return reply.list;
}

// runs periodically and prints stats (active/completed/overdue)
void Monitor_Task(void *pv)
{
	TickType_t last = xTaskGetTickCount();
	uint32_t an, cn, on;
	dd_task_list *a_list, *c_list, *o_list;
	(void)pv;

	for (;;) {
		vTaskDelayUntil(&last, pdMS_TO_TICKS(1500));

		a_list = get_active_dd_task_list();
		c_list = get_completed_dd_task_list();
		o_list = get_overdue_dd_task_list();

		an = get_list_size(a_list);
		cn = get_list_size(c_list);
		on = get_list_size(o_list);

		sem_printf("M:a-%u c-%u o-%u\r\n",
				   (unsigned int)an,
				   (unsigned int)cn,
				   (unsigned int)on);
	}
}

// actual task 1 workload
// waits for release signal then does busy work then completes
void User_Defined_Task_1(void *pv)
{
	uint32_t iid; (void)pv;
	for (;;) {
		xQueueReceive(task1_release_queue, &iid, portMAX_DELAY);
		busy_wait_ms(T1_EXEC_MS);
		complete_dd_task(iid);
	}
}

// same as task 1 but different execution time
// simulates another periodic task
void User_Defined_Task_2(void *pv)
{
	uint32_t iid; (void)pv;
	for (;;) {
		xQueueReceive(task2_release_queue, &iid, portMAX_DELAY);
		busy_wait_ms(T2_EXEC_MS);
		complete_dd_task(iid);
	}
}

// third task  same pattern
// just different timing params
void User_Defined_Task_3(void *pv)
{
	uint32_t iid; (void)pv;
	for (;;) {
		xQueueReceive(task3_release_queue, &iid, portMAX_DELAY);
		busy_wait_ms(T3_EXEC_MS);
		complete_dd_task(iid);
	}
}

// returns correct queue for each task id
// used when releasing tasks
static QueueHandle_t get_release_queue(uint32_t id)
{
	switch (id) {
		case 1u: return task1_release_queue;
		case 2u: return task2_release_queue;
		case 3u: return task3_release_queue;
		default: return NULL;
	}
}

// maps task id to its handle
// needed when sending release info to scheduler
static TaskHandle_t get_task_handle_for(uint32_t id)
{
	switch (id) {
		case 1u: return task1_handle;
		case 2u: return task2_handle;
		case 3u: return task3_handle;
		default: return NULL;
	}
}

// returns period in ticks for each task
// converts ms macros to RTOS ticks
static TickType_t get_period_ticks(uint32_t id)
{
	switch (id) {
		case 1u: return pdMS_TO_TICKS(T1_PERIOD_MS);
		case 2u: return pdMS_TO_TICKS(T2_PERIOD_MS);
		case 3u: return pdMS_TO_TICKS(T3_PERIOD_MS);
		default: return 0;
	}
}

// finds earliest next release among all tasks
// used to set timer correctly
static TickType_t get_next_release_min(void)
{
	TickType_t n = t1_next_release_tick;
	if (t2_next_release_tick < n) n = t2_next_release_tick;
	if (t3_next_release_tick < n) n = t3_next_release_tick;
	return n;
}

// releases a single task instance
// calculates relative deadline and sends it to DDS
static void release_one_task(uint32_t id, task_type type,
							TickType_t rel, TickType_t dl)
{
	QueueHandle_t q = get_release_queue(id);
	TaskHandle_t h = get_task_handle_for(id);
	TickType_t relative_deadline = dl - rel;
	uint32_t iid;

	if (q == NULL || h == NULL) return;

	iid = release_dd_task(h, type, id, (uint32_t)relative_deadline);
	xQueueSend(q, &iid, portMAX_DELAY);
}

// checks if any task should be released now (or overdue release)
// loops to catch up if multiple releases missed
static void release_due_tasks(void)
{
	TickType_t now = xTaskGetTickCount();

	while (t1_next_release_tick <= now) {
		release_one_task(1u, PERIODIC, t1_next_release_tick, t1_next_deadline_tick);
		t1_next_release_tick += get_period_ticks(1u);
		t1_next_deadline_tick += get_period_ticks(1u);
	}
	while (t2_next_release_tick <= now) {
		release_one_task(2u, PERIODIC, t2_next_release_tick, t2_next_deadline_tick);
		t2_next_release_tick += get_period_ticks(2u);
		t2_next_deadline_tick += get_period_ticks(2u);
	}
	while (t3_next_release_tick <= now) {
		release_one_task(3u, PERIODIC, t3_next_release_tick, t3_next_deadline_tick);
		t3_next_release_tick += get_period_ticks(3u);
		t3_next_deadline_tick += get_period_ticks(3u);
	}
}

// sets timer for next release event
// basically schedules generator wakeup
static void arm_release_timer(void)
{
	TickType_t next = get_next_release_min();
	TickType_t now = xTaskGetTickCount();
	TickType_t delay = (next <= now) ? 1u : (next - now);
	xTimerChangePeriod(release_timer, delay, 0);
}

// timer callback just sends token to generator task
// cant do heavy work here so just notify
static void release_timer_cb(TimerHandle_t xTimer)
{
	uint32_t tok = GENERATOR_TOKEN;
	(void)xTimer;
	xQueueSend(generator_queue, &tok, 0);
}

// generates periodic tasks releases
// manages release timing using timer + queues
void DD_Generator_Task(void *pv)
{
	TickType_t t0;
	uint32_t tok;
	(void)pv;

	task1_release_queue = xQueueCreate(4, sizeof(uint32_t));
	task2_release_queue = xQueueCreate(4, sizeof(uint32_t));
	task3_release_queue = xQueueCreate(4, sizeof(uint32_t));

	xTaskCreate(User_Defined_Task_1, "T1", configMINIMAL_STACK_SIZE + 128,
				NULL, PRIORITY_USER_LOW, &task1_handle);
	xTaskCreate(User_Defined_Task_2, "T2", configMINIMAL_STACK_SIZE + 128,
				NULL, PRIORITY_USER_LOW, &task2_handle);
	xTaskCreate(User_Defined_Task_3, "T3", configMINIMAL_STACK_SIZE + 128,
				NULL, PRIORITY_USER_LOW, &task3_handle);

	t0 = xTaskGetTickCount();
	t1_next_release_tick = t0;
	t1_next_deadline_tick = t0 + pdMS_TO_TICKS(T1_PERIOD_MS);
	t2_next_release_tick = t0;
	t2_next_deadline_tick = t0 + pdMS_TO_TICKS(T2_PERIOD_MS);
	t3_next_release_tick = t0;
	t3_next_deadline_tick = t0 + pdMS_TO_TICKS(T3_PERIOD_MS);

	release_timer = xTimerCreate("RelTmr", pdMS_TO_TICKS(1),
								 pdFALSE, (void *)0, release_timer_cb);

	release_due_tasks();
	arm_release_timer();

	for (;;) {
		xQueueReceive(generator_queue, &tok, portMAX_DELAY);
		release_due_tasks();
		arm_release_timer();
	}
}

// sets everything up (queues, tasks, scheduler)
// then starts RTOS scheduler
int main(void)
{
	dwt_init();

	dds_queue = xQueueCreate(16, sizeof(dds_message));
	generator_queue = xQueueCreate(8, sizeof(uint32_t));
	print_mutex = xSemaphoreCreateMutex();

	xTaskCreate(DD_Scheduler_Task, "DDS", configMINIMAL_STACK_SIZE,
				NULL, PRIORITY_DDS, NULL);
	xTaskCreate(Monitor_Task, "Mon", configMINIMAL_STACK_SIZE,
				NULL, PRIORITY_MONITOR, NULL);
	xTaskCreate(DD_Generator_Task, "Gen", configMINIMAL_STACK_SIZE,
				NULL, PRIORITY_GENERATOR, NULL);

	vTaskStartScheduler();
	while (1) {}
}
// called if malloc fails  just stuck in loop (bad but ok for debug)
void vApplicationMallocFailedHook(void)										{ for (;;); }

// called if stack overflow happens
// again just infinite loop so we notice crash
void vApplicationStackOverflowHook(xTaskHandle t, signed char *n)			{ (void)t; (void)n; for (;;); }

// runs when idle  just checking free heap for debug
// not doing much honestly
void vApplicationIdleHook(void) { volatile size_t s = xPortGetFreeHeapSize(); (void)s; }
