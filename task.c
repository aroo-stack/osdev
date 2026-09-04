#include "task.h"
#include "pmm.h"
#include "paging.h"
#include <stdint.h>
#include <stddef.h>

task_t tasks[MAX_TASKS];
int num_tasks = 0;
int current_task = 0;
task_t *task_list[MAX_TASKS];

static inline void outb(uint16_t port, uint8_t v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(port));}
static inline uint8_t inb(uint16_t port){ uint8_t r; __asm__ volatile("inb %1,%0":"=a"(r):"Nd"(port)); return r;}
static int tx_empty(){ return inb(0x3F8+5)&0x20; }
static void s_putc(char c){ while(!tx_empty()); outb(0x3F8,c); }
static void s_puts(const char*s){ for(size_t i=0;s[i];i++) s_putc(s[i]); }
static void s_put_dec(int n){ char b[12]; int i=0; if(n==0){s_putc('0');return;} int neg=0; if(n<0){neg=1;n=-n;} while(n){b[i++]='0'+n%10; n/=10;} if(neg) s_putc('-'); while(i--) s_putc(b[i]); }
static void s_put_hex(uint32_t n){ s_puts("0x"); for(int i=28;i>=0;i-=4){ uint8_t v=(n>>i)&0xF; s_putc(v<10?'0'+v:'A'+v-10); } }

void task_init(void){
    uint32_t esp;
    __asm__ volatile("mov %%esp, %0":"=r"(esp));
    tasks[0].id = 0;
    tasks[0].esp = esp;
    tasks[0].state = TASK_RUNNING;
    tasks[0].stack_base = 0;
    tasks[0].stack_top = esp;
    for(int i=0;i<16;i++) tasks[0].name[i]=0;
    const char *n="GUI";
    for(int i=0;n[i] && i<16;i++) tasks[0].name[i]=n[i];
    task_list[0] = &tasks[0];
    num_tasks = 1;
    current_task = 0;
    s_puts("TASK: init task 0 GUI esp="); s_put_hex(esp); s_puts("\n");
}

int task_create(void (*entry)(void), const char *name){
    if(num_tasks >= MAX_TASKS) return -1;
    // Avoid overlap with back buffer at 0x01000000 (16MB, 8.3M) and heap at 0x00400000
    // Use 0x03000000 (48MB) for task stacks, which is well beyond back buffer (0x01000000-0x018E9000) and wallpaper cache at 0x02000000
    // Previously 0x00E00000 overlapped for 1920x1080 8M buffers, so moved to 0x03000000
    uint32_t vbase = 0x03000000 + num_tasks * TASK_STACK_SIZE;
    paging_ensure_range(vbase, TASK_STACK_SIZE);
    for(int i=0;i<TASK_STACK_SIZE;i+=4096){
        uint32_t v = vbase + i;
        uint32_t p = pmm_alloc_frame();
        if(!p){ s_puts("TASK: out of frames\n"); return -1; }
        paging_map(v, p, 0x03);
        volatile uint32_t *ptr = (volatile uint32_t*)v;
        for(int j=0;j<1024;j++) ptr[j]=0;
    }
    uint32_t stack_top = vbase + TASK_STACK_SIZE;
    // Build initial stack frame as if the task had been interrupted
    // The PIT handler's prologue after CPU push (EFLAGS,CS,EIP) and irq stub (vector,err) does:
    // pusha (8 regs), push ds, then save ESP, then on restore pop ds, popa, add esp 8, iret
    // So the new task's stack must contain, from low to high:
    // [esp] ds (0x10)
    // [esp+4] edi, [esp+8] esi, [esp+12] ebp, [esp+16] esp, [esp+20] ebx, [esp+24] edx, [esp+28] ecx, [esp+32] eax
    // [esp+36] vector (32), [esp+40] error (0), [esp+44] EIP (entry), [esp+48] CS (0x08), [esp+52] EFLAGS (0x202)
    uint32_t *stack = (uint32_t*)stack_top;
    stack--; *stack = 0x202; // EFLAGS
    stack--; *stack = 0x08;  // CS
    stack--; *stack = (uint32_t)entry; // EIP
    stack--; *stack = 0; // error code
    stack--; *stack = 32; // vector
    // pusha: eax, ecx, edx, ebx, esp, ebp, esi, edi - popa will pop edi,esi,ebp,esp,ebx,edx,ecx,eax
    // Push in reverse order of popa: edi, esi, ebp, esp, ebx, edx, ecx, eax from low to high?
    // Actually pusha pushes eax,ecx,edx,ebx,esp,ebp,esi,edi with edi at lowest, so stack after pusha has edi at [esp], eax at [esp+28]
    // For our pre-filled stack, we need to have the same layout: at [esp] edi, [esp+28] eax
    // So we push in order that will be popped correctly: push eax last so it's at highest among pusha
    // Simpler: just push 8 zeros in the order that popa expects
    // popa pops edi,esi,ebp,esp,ebx,edx,ecx,eax - so the stack should have edi at lowest, eax at highest among those 8
    // That means we should push eax first, then ecx, etc., with edi last, so that after pushes, edi is at top
    // But our stack grows down, so pushing in order eax, ecx, edx, ebx, esp, ebp, esi, edi will result in edi at top
    // Let's just push 8 zeros in that order
    stack--; *stack = 0; // eax
    stack--; *stack = 0; // ecx
    stack--; *stack = 0; // edx
    stack--; *stack = 0; // ebx
    stack--; *stack = 0; // esp (dummy, will be discarded by popa)
    stack--; *stack = 0; // ebp
    stack--; *stack = 0; // esi
    stack--; *stack = 0; // edi
    // Segment registers: handler pushes ds (old ds) as eax, then sets ds to 0x10
    // For new task, we want ds=0x10, so push 0x10 as the saved ds value
    stack--; *stack = 0x10; // ds
    // Also need to account for the fact that handler pushes ds, then does mov ax,0x10; mov ds,ax etc., but only ds is saved, es/fs/gs are set to 0x10 without saving their old values separately?
    // In isr_common, it does: pusha; mov ax,ds; push eax; mov ds,0x10; mov es,0x10 etc., then later pop ebx; mov ds,bx etc.
    // So only ds is saved, es/fs/gs are not saved separately, they are just set to 0x10 and restored from the same saved ds value
    // So our stack only needs one ds value, not 4, for the isr_common path. But our pit_handler does push ds, es, fs, gs separately (4 pushes)
    // For pit_handler, it does: pusha; push ds; push es; push fs; push gs (4 segment pushes)
    // So we need to match that: pit_handler pushes 4 segment registers, not just ds
    // Let's adjust: pit_handler pushes ds, then sets all to 0x10, so the stack has gs,fs,es,ds? Actually it pushes ds, then sets ds,es,fs,gs to 0x10, so the stack has ds at top, but es/fs/gs are not pushed?
    // Wait, pit_handler as we wrote does: pusha; mov ax,ds; push eax; mov ds,0x10; mov es,0x10; mov fs,0x10; mov gs,0x10
    // So it only pushes one segment register (ds), not 4. Then on restore it does: pop ebx; mov ds,bx; mov es,bx; mov fs,bx; mov gs,bx; popa
    // So only one segment value is saved, and it restores all 4 from that one.
    // So for new task, we only need to push one ds value (0x10) as the saved segment
    // But we already pushed ds as 0x10 above, that's the one that will be popped into ebx and then moved to all segments
    // So the stack for new task should have: ds (0x10), then the 8 pusha regs, then vector, error, EIP, CS, EFLAGS
    // We already pushed ds as 0x10, and 8 zeros for pusha, and vector/error/EIP/CS/EFLAGS, so that's correct
    // The stack pointer for this task should be at the ds position (lowest)
    uint32_t task_esp = (uint32_t)stack;
    tasks[num_tasks].id = num_tasks;
    tasks[num_tasks].esp = task_esp;
    tasks[num_tasks].stack_base = (uint8_t*)vbase;
    tasks[num_tasks].stack_top = stack_top;
    tasks[num_tasks].state = TASK_READY;
    for(int i=0;i<16;i++) tasks[num_tasks].name[i]=0;
    for(int i=0; name[i] && i<16;i++) tasks[num_tasks].name[i]=name[i];
    s_puts("TASK: created "); s_puts(name); s_puts(" id "); s_put_dec(num_tasks); s_puts(" esp "); s_put_hex(task_esp); s_puts("\n");
    task_list[num_tasks] = &tasks[num_tasks];
    num_tasks++;
    return 0;
}

task_t* task_current(void){ return task_list[current_task]; }
int task_count(void){ return num_tasks; }

static int pit_ticks = 0;
static int task_ticks[MAX_TASKS] = {0};
static int mouse_irqs = 0;
volatile int g_report_ticks = 0;
void scheduler_tick(void){
    pit_ticks++;
    // Priority: GUI (task 0) gets 50% (every other tick), A and B share remaining 50% (25% each)
    // This fixes jitter: GUI was getting only 33% at 100Hz = 33Hz, not enough for 60fps drag
    // Now GUI gets 50Hz, enough for smooth 60fps, while A/B still make progress but yield via hlt
    int next;
    // Simple pattern: 0,1,0,2,0,1,0,2...
    static int phase = 0;
    if(phase % 2 == 0){
        next = 0; // GUI
    } else {
        // Alternate between A and B for the non-GUI ticks
        static int ab_turn = 0;
        next = (ab_turn % 2 == 0) ? 1 : 2;
        ab_turn++;
    }
    phase++;
    // Fallback to simple round-robin if num_tasks !=3 or for other cases
    if(num_tasks != 3){
        next = (current_task + 1) % num_tasks;
    }
    task_ticks[next]++;
    if(g_in_redraw){
        extern volatile int g_sched_during_redraw;
        g_sched_during_redraw++;
    }
    if(pit_ticks % 100 == 0){
        extern volatile int g_report_ticks;
        g_report_ticks = 1;
    }
    current_task = next;
}
int pit_get_ticks(void){ return pit_ticks; }
void pit_get_task_ticks(int *out_gui, int *out_a, int *out_b){
    if(out_gui) *out_gui = task_ticks[0];
    if(out_a) *out_a = task_ticks[1];
    if(out_b) *out_b = task_ticks[2];
}
void pit_reset_task_ticks(void){
    for(int i=0;i<MAX_TASKS;i++) task_ticks[i]=0;
}
void mouse_irq_count_inc(void){ mouse_irqs++; }
int mouse_get_irqs(void){ return mouse_irqs; }
void mouse_reset_irqs(void){ mouse_irqs=0; }

void task_yield(void){ __asm__ volatile("int $32"); }
void scheduler_start(void){ s_puts("TASK: scheduler ready\n"); }
