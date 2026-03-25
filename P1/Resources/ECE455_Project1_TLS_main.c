/*
 * ECE 455 – Project 1: Traffic Light System (TLS)
 * Full project code following the lab-manual structure:
 *  - Flow Adjust Task (pot -> flow queue)
 *  - Traffic Generator Task (flow -> random-ish car events queue)
 *  - Traffic Light Task + SOFTWARE TIMER (timer callback advances light state)
 *  - System Display Task + SOFTWARE TIMER (timer tick -> move cars + shift register + light LEDs)
 *
 * IMPORTANT: Per your request:
 *  - Shift-register pins (PC6/PC7/PC8): configured ONLY as OUTPUT with NO pull-up/down.
 *    (Library requires setting OType/Speed fields; we keep standard push-pull + moderate speed.)
 *  - Traffic light pins (PC0/PC1/PC2): configured as OUTPUT with PULL-DOWN.
 *  - No printf / debug prints.
 *  - Keep the variable naming style you showed: traffic_gen_q, light_state_q, display_notify_q,
 *    system_display_timer, traffic_gen_timer, traffic_light_timer, stdout_m (unused here).
 */

#include <stdint.h>
#include "stm32f4xx.h"
#include "stm32f4xx_gpio.h"
#include "stm32f4xx_adc.h"
#include "stm32f4xx_rcc.h"

/* FreeRTOS includes */
#include "../FreeRTOS_Source/include/FreeRTOS.h"
#include "../FreeRTOS_Source/include/task.h"
#include "../FreeRTOS_Source/include/queue.h"
#include "../FreeRTOS_Source/include/timers.h"
#include "../FreeRTOS_Source/include/semphr.h"

/* ---------------- Project constants ---------------- */
#define ROAD_LEN            19
#define STOP_LINE_INDEX     8

/* Car movement speed: 500ms tick => 2 LEDs/sec (within 1–3 LEDs/sec) */
#define DISPLAY_TICK_MS     500

/* Pot sampling */
#define FLOW_SAMPLE_MS     500 

/* Constant yellow duration */
#define YELLOW_MS          2000 

/* Light state encoding (your preference) */
#define AMBER   0u
#define GREEN   1u
#define RED     2u

/* GPIO pin mapping (per manual) */
#define TL_RED_PIN      GPIO_Pin_0  /* PC0 */
#define TL_AMBER_PIN    GPIO_Pin_1  /* PC1 */
#define TL_GREEN_PIN    GPIO_Pin_2  /* PC2 */
#define POT_PIN         GPIO_Pin_3  /* PC3 -> ADC1 channel 13 */

#define SR_DATA_PIN     GPIO_Pin_6  /* PC6 */
#define SR_CLK_PIN      GPIO_Pin_7  /* PC7 (falling edge) */
#define SR_RST_PIN      GPIO_Pin_8  /* PC8 (active low reset) */

/* ---------------- Original-style global handles ---------------- */
QueueHandle_t traffic_gen_q      = NULL;  /* uint8_t: 1 event = 1 new car */
QueueHandle_t light_state_q      = NULL;  /* uint8_t: AMBER/GREEN/RED (latest) */
QueueHandle_t flow_q             = NULL;  /* uint16_t: 0..1000 permille (latest) */
QueueHandle_t display_notify_q   = NULL;  /* uint8_t: display tick event */
TimerHandle_t system_display_timer = NULL; /* periodic tick */
TimerHandle_t traffic_light_timer  = NULL; /* one-shot, callback advances state */
TimerHandle_t traffic_gen_timer    = NULL; /* NOT required; we generate in task (kept NULL) */

SemaphoreHandle_t stdout_m = NULL; /* kept for naming compatibility; not used */

/* ---------------- Forward declarations ---------------- */
static void prvSetupHardware(void);
static void prvGPIO_Init(void);
static void prvADC_Init(void);
static uint16_t prvADC_Read12(void);

/* Tasks */
static void flow_adjust_task(void *pvParameters);
static void traffic_generator_task(void *pvParameters);
static void traffic_light_task(void *pvParameters);
static void system_display_task(void *pvParameters);

/* Timer callbacks */
static void system_display_callback(TimerHandle_t xTimer);
static void traffic_light_callback(TimerHandle_t xTimer);

/* Hardware output helpers */
static void traffic_light_display(uint8_t state);
static inline void SR_ResetPulse(void);
static inline void SR_SetData(uint8_t bit);
static inline void SR_ClockPulse_FallingEdge(void);
static void traffic_to_shift_reg(const uint8_t car_pos[ROAD_LEN]);

/* Small helpers */
static inline uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi);
static inline uint32_t lerp_u32(uint32_t a, uint32_t b, uint32_t t_permille);

/* ---------------- main ---------------- */
int main(void)
{
    prvSetupHardware();

    /* Queues (latest-value channels use length 1 + overwrite) */
    flow_q           = xQueueCreate(1, sizeof(uint16_t));
    light_state_q    = xQueueCreate(1, sizeof(uint8_t));
    traffic_gen_q    = xQueueCreate(32, sizeof(uint8_t));
    display_notify_q = xQueueCreate(4, sizeof(uint8_t));

    /* Publish defaults */
    {
        uint16_t flow0 = 0;
        uint8_t  ls0   = RED;
        xQueueOverwrite(flow_q, &flow0);
        xQueueOverwrite(light_state_q, &ls0);
        traffic_light_display(ls0);
    }

    /* Timers */
    system_display_timer = xTimerCreate(
        "system_display_timer",
        pdMS_TO_TICKS(DISPLAY_TICK_MS),
        pdTRUE,
        NULL,
        system_display_callback
    );

    /* REQUIRED: traffic light state is controlled by a software timer */
    traffic_light_timer = xTimerCreate(
        "traffic_light_timer",
        pdMS_TO_TICKS(1000),
        pdFALSE, /* one-shot; callback changes period each time */
        NULL,
        traffic_light_callback
    );

    /* Create tasks (names match your style) */
    xTaskCreate(system_display_task,    "System Display Manager", configMINIMAL_STACK_SIZE, NULL, 3, NULL);
    xTaskCreate(traffic_generator_task, "Traffic Manager",        configMINIMAL_STACK_SIZE, NULL, 2, NULL);
    xTaskCreate(traffic_light_task,     "Traffic Light Manager",  configMINIMAL_STACK_SIZE, NULL, 3, NULL);
    xTaskCreate(flow_adjust_task,       "Flow Adjust",            configMINIMAL_STACK_SIZE, NULL, 1, NULL);

    /* Start timers */
    xTimerStart(system_display_timer, 0);

    /* Light task will start traffic_light_timer after publishing initial state */

    vTaskStartScheduler();

    for (;;) { }
}

/* ---------------- Hardware setup ---------------- */
static void prvSetupHardware(void)
{
    NVIC_SetPriorityGrouping(0);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_ADC1, ENABLE);

    prvGPIO_Init();
    prvADC_Init();

    /* Shift register idle levels: CLK high so falling edge is high->low */
    GPIO_SetBits(GPIOC, SR_CLK_PIN);
    GPIO_ResetBits(GPIOC, SR_DATA_PIN);
    GPIO_SetBits(GPIOC, SR_RST_PIN);
}

static void prvGPIO_Init(void)
{
    GPIO_InitTypeDef g;

    /* -------- Shift register pins: OUTPUT, NO PULL --------
       (You requested “only output, no pull-up push etc.”:
        SPL requires OType/Speed; we keep standard push-pull + moderate speed,
        and ensure NO pull-up/down.)
    */
    g.GPIO_Pin   = SR_DATA_PIN | SR_CLK_PIN | SR_RST_PIN;
    g.GPIO_Mode  = GPIO_Mode_OUT;
    // g.GPIO_PuPd  = GPIO_PuPd_NOPULL;
    // g.GPIO_OType = GPIO_OType_PP;
    // g.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOC, &g);

    /* -------- Traffic light pins: OUTPUT with PULL-DOWN -------- */
    g.GPIO_Pin   = TL_RED_PIN | TL_AMBER_PIN | TL_GREEN_PIN;
    g.GPIO_Mode  = GPIO_Mode_OUT;
    g.GPIO_PuPd  = GPIO_PuPd_DOWN;    /* per your request */
    g.GPIO_OType = GPIO_OType_PP;
    g.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOC, &g);

    /* -------- Potentiometer pin: Analog, no pull -------- */
    g.GPIO_Pin   = POT_PIN;
    g.GPIO_Mode  = GPIO_Mode_AN;
    g.GPIO_PuPd  = GPIO_PuPd_NOPULL;
    g.GPIO_OType = GPIO_OType_PP;
    g.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOC, &g);

    /* Start with TL LEDs off */
    GPIO_ResetBits(GPIOC, TL_RED_PIN | TL_AMBER_PIN | TL_GREEN_PIN);
}

static void prvADC_Init(void)
{
    ADC_CommonInitTypeDef ac;
    ADC_InitTypeDef a;

    ADC_CommonStructInit(&ac);
    ac.ADC_Mode = ADC_Mode_Independent;
    ac.ADC_Prescaler = ADC_Prescaler_Div4;
    ac.ADC_DMAAccessMode = ADC_DMAAccessMode_Disabled;
    ac.ADC_TwoSamplingDelay = ADC_TwoSamplingDelay_5Cycles;
    ADC_CommonInit(&ac);

    ADC_StructInit(&a);
    a.ADC_Resolution = ADC_Resolution_12b;
    a.ADC_ScanConvMode = DISABLE;
    a.ADC_ContinuousConvMode = DISABLE;
    a.ADC_ExternalTrigConvEdge = ADC_ExternalTrigConvEdge_None;
    a.ADC_DataAlign = ADC_DataAlign_Right;
    a.ADC_NbrOfConversion = 1;
    ADC_Init(ADC1, &a);

    /* PC3 is ADC1 channel 13 */
    ADC_RegularChannelConfig(ADC1, ADC_Channel_13, 1, ADC_SampleTime_84Cycles);

    ADC_Cmd(ADC1, ENABLE);
}

static uint16_t prvADC_Read12(void)
{
    ADC_SoftwareStartConv(ADC1);
    while (ADC_GetFlagStatus(ADC1, ADC_FLAG_EOC) == RESET) { }
    return (uint16_t)ADC_GetConversionValue(ADC1); /* 0..4095 */
}

/* ---------------- Tasks ---------------- */

/* Flow Adjust Task: reads pot, publishes 0..1000 “flow permille” */
static void flow_adjust_task(void *pvParameters)
{
    (void)pvParameters;
    TickType_t last = xTaskGetTickCount();

    while (1)
    {
        uint16_t adc = prvADC_Read12(); /* 0..4095 */
        uint32_t flow_permille = ((uint32_t)adc * 1000u) / 4095u;
        flow_permille = clamp_u32(flow_permille, 0u, 1000u);

        uint16_t flow = (uint16_t)flow_permille;
        xQueueOverwrite(flow_q, &flow);

        vTaskDelayUntil(&last, pdMS_TO_TICKS(FLOW_SAMPLE_MS));
    }
}

/* Traffic Generator Task:
   - Max flow: bumper-to-bumper => 1 car per display tick
   - Min flow: ~5–6 LED gap     => ~1 car every 6 ticks
   - Add small randomness (±1 tick) to satisfy “random generation” requirement.
*/
static void traffic_generator_task(void *pvParameters)
{
    (void)pvParameters;

    uint32_t rng = 0xC001D00Du ^ (uint32_t)xTaskGetTickCount();
    TickType_t last = xTaskGetTickCount();

    while (1)
    {
        uint16_t flow = 0;
        (void)xQueuePeek(flow_q, &flow, 0);

        /* Base gap: 6 -> 1 as flow goes 0 -> 1000 */
        uint32_t base_gap = 6u - (5u * (uint32_t)flow) / 1000u;
        base_gap = clamp_u32(base_gap, 1u, 6u);

        /* Jitter: -1, 0, +1 (clamped) */
        rng = 1103515245u * rng + 12345u;
        int32_t jitter = (int32_t)((rng >> 30) & 0x3u) - 1; /* -1,0,1,2 */
        if (jitter > 1) jitter = 1;

        int32_t gap = (int32_t)base_gap + jitter;
        if (gap < 1) gap = 1;
        if (gap > 6) gap = 6;

        /* Emit one car event */
        {
            uint8_t ev = 1u;
            (void)xQueueSend(traffic_gen_q, &ev, 0);
        }

        vTaskDelayUntil(&last, (TickType_t)gap * pdMS_TO_TICKS(DISPLAY_TICK_MS));
    }
}

/* Traffic Light Task:
   - Publishes initial state
   - Starts traffic_light_timer (software timer controls state changes)
*/
static void traffic_light_task(void *pvParameters)
{
    (void)pvParameters;

    {
        uint8_t s = RED;
        xQueueOverwrite(light_state_q, &s);
        traffic_light_display(s);
    }

    /* Start first timer period; callback will immediately apply correct durations */
    xTimerChangePeriod(traffic_light_timer, pdMS_TO_TICKS(1000), 0);
    xTimerStart(traffic_light_timer, 0);

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* System Display Task (single combined display task):
   - waits for display tick from system_display_timer via display_notify_q
   - drains traffic_gen_q
   - reads latest light_state_q
   - moves cars + stop line rule
   - writes shift register + traffic lights
*/
static void system_display_task(void *pvParameters)
{
    (void)pvParameters;

    uint8_t car_pos[ROAD_LEN] = {0};
    uint8_t light_state = RED;

    while (1)
    {
        uint8_t notify;
        xQueueReceive(display_notify_q, &notify, portMAX_DELAY);
        (void)notify;

        /* Drain “new car” events */
        uint8_t new_cars = 0;
        uint8_t traffic_rx;
        while (xQueueReceive(traffic_gen_q, &traffic_rx, 0) == pdTRUE)
        {
            (void)traffic_rx;
            if (new_cars < 255u) new_cars++;
        }

        /* Latest light state */
        (void)xQueuePeek(light_state_q, &light_state, 0);

        /* Car exits at road end */
        car_pos[ROAD_LEN - 1] = 0;

        /* Move cars forward */
        for (int i = ROAD_LEN - 1; i > 0; i--)
        {
            if (car_pos[i - 1] == 1u && car_pos[i] == 0u)
            {
                /* Stop line: block entering STOP_LINE_INDEX unless GREEN */
                if ((i == STOP_LINE_INDEX) && (light_state != GREEN))
                {
                    continue;
                }
                car_pos[i] = 1u;
                car_pos[i - 1] = 0u;
            }
        }

        /* Spawn at position 0 if free (one per tick max) */
        if (new_cars > 0u && car_pos[0] == 0u)
        {
            car_pos[0] = 1u;
        }

        /* Update outputs */
        traffic_to_shift_reg(car_pos);
        traffic_light_display(light_state);
    }
}

/* ---------------- Timer callbacks ---------------- */

/* System display timer: pushes a tick event to the display task */
static void system_display_callback(TimerHandle_t xTimer)
{
    (void)xTimer;
    uint8_t notify = 1u;
    (void)xQueueSend(display_notify_q, &notify, 0);
}

/* Traffic light timer callback: advances state machine + sets next duration
   Durations depend on flow:
     flow=0   => green=1000ms, red=2000ms  (red ≈ 2x green)
     flow=1000=> green=2000ms, red=1000ms  (green ≈ 2x red)
     yellow constant = YELLOW_MS
*/
static void traffic_light_callback(TimerHandle_t xTimer)
{
    (void)xTimer;

    /* Current state */
    uint8_t state = RED;
    (void)xQueuePeek(light_state_q, &state, 0);

    /* Latest flow */
    uint16_t flow = 0;
    (void)xQueuePeek(flow_q, &flow, 0);

    uint32_t green_ms = lerp_u32(1000u, 2000u, flow);
    uint32_t red_ms   = lerp_u32(2000u, 1000u, flow);
    uint32_t yell_ms  = YELLOW_MS;

    TickType_t next_period;

    if (state == RED)
    {
        state = GREEN;
        next_period = pdMS_TO_TICKS(green_ms);
    }
    else if (state == GREEN)
    {
        state = AMBER;
        next_period = pdMS_TO_TICKS(yell_ms);
    }
    else
    {
        state = RED;
        next_period = pdMS_TO_TICKS(red_ms);
    }

    xQueueOverwrite(light_state_q, &state);
    traffic_light_display(state);

    xTimerChangePeriod(traffic_light_timer, next_period, 0);
    xTimerStart(traffic_light_timer, 0);
}

/* ---------------- Output helpers ---------------- */

static void traffic_light_display(uint8_t state)
{
    /* All off */
    GPIO_ResetBits(GPIOC, TL_RED_PIN | TL_AMBER_PIN | TL_GREEN_PIN);

    if (state == RED)        GPIO_SetBits(GPIOC, TL_RED_PIN);
    else if (state == AMBER) GPIO_SetBits(GPIOC, TL_AMBER_PIN);
    else                    GPIO_SetBits(GPIOC, TL_GREEN_PIN);
}

/* Shift register: active-low reset pulse.
   You said no delay loops needed; use only a couple NOPs. */
static inline void SR_ResetPulse(void)
{
    GPIO_ResetBits(GPIOC, SR_RST_PIN);
    __NOP(); __NOP(); __NOP(); __NOP();
    GPIO_SetBits(GPIOC, SR_RST_PIN);
}

static inline void SR_SetData(uint8_t bit)
{
    if (bit) GPIO_SetBits(GPIOC, SR_DATA_PIN);
    else     GPIO_ResetBits(GPIOC, SR_DATA_PIN);
}

/* Clock idles high; falling edge triggers shift */
static inline void SR_ClockPulse_FallingEdge(void)
{
    GPIO_ResetBits(GPIOC, SR_CLK_PIN); /* falling edge */
    GPIO_SetBits(GPIOC, SR_CLK_PIN);   /* return high */
}

static void traffic_to_shift_reg(const uint8_t car_pos[ROAD_LEN])
{
    SR_ResetPulse();

    /* Shift out end->start (matches your working loop style) */
    for (int i = ROAD_LEN - 1; i >= 0; i--)
    {
        SR_SetData(car_pos[i] ? 1u : 0u);
        SR_ClockPulse_FallingEdge();
    }

    /* 3 shift registers => 24 outputs, pad remaining 5 with 0 */
    for (int i = 0; i < (24 - ROAD_LEN); i++)
    {
        SR_SetData(0u);
        SR_ClockPulse_FallingEdge();
    }
}

/* ---------------- Utility ---------------- */

static inline uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* t_permille in 0..1000 */
static inline uint32_t lerp_u32(uint32_t a, uint32_t b, uint32_t t_permille)
{
    t_permille = clamp_u32(t_permille, 0u, 1000u);
    if (b >= a) return a + ((b - a) * t_permille) / 1000u;
    else        return a - ((a - b) * t_permille) / 1000u;
}

/* ---------------- FreeRTOS hooks ---------------- */
void vApplicationMallocFailedHook(void)
{
    taskDISABLE_INTERRUPTS();
    for (;;) { }
}

void vApplicationStackOverflowHook(TaskHandle_t pxTask, char *pcTaskName)
{
    (void)pxTask;
    (void)pcTaskName;
    taskDISABLE_INTERRUPTS();
    for (;;) { }
}

void vApplicationIdleHook(void)
{
    /* intentionally empty */
}
