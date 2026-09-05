#ifndef TASK_H
#define TASK_H
#include <stdint.h>

#define TASK_STACK_SIZE 8192
#define MAX_TASKS 4

#define TASK_READY 0
#define TASK_RUNNING 1
// NOTE: pause/resume was removed (TASK_PAUSED deleted). All live tasks are always
// runnable; Kill is the only task control. See git history for the removed feature.

typedef struct task {
    uint32_t esp;
    uint32_t ebp;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax, ebx, ecx, edx, esi, edi;
    uint16_t ds, es, fs, gs, ss, cs;
    uint8_t *stack_base;
    uint32_t stack_top;
    uint32_t stack_paddrs[2]; // physical frames for stack pages, for free on kill
    int id;
    int state;
    char name[16];
} task_t;

void task_init(void);
int task_create(void (*entry)(void), const char *name);
int task_create_with_id(int want_id, void (*entry)(void), const char *name);
void task_yield(void);
task_t* task_current(void);
int task_count(void);
void scheduler_tick(void);
void scheduler_start(void);
extern volatile int g_in_redraw;
extern volatile int g_needs_redraw;
extern int current_task;
extern task_t *task_list[MAX_TASKS];
int pit_get_ticks(void);
void pit_get_task_ticks(int *gui, int *a, int *b);
void pit_reset_task_ticks(void);
void pit_get_cpu_percent(int *gui_pct, int *a_pct, int *b_pct);
int task_kill(int id); // remove from scheduler, free stack, close window - returns 1 if killed
int task_find_by_name(const char *name);
int task_kill_by_name(const char *name);
int task_exists(int id);
void task_clicker_entry(void);
void task_notes_entry(void);
void mouse_irq_count_inc(void);
int mouse_get_irqs(void);
void mouse_reset_irqs(void);
extern volatile int g_report_ticks;

#endif
