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
static inline uint64_t rdtsc(void){ uint32_t lo,hi; __asm__ volatile("rdtsc":"=a"(lo),"=d"(hi)); return ((uint64_t)hi<<32)|lo; }
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

// Permanent-ID lookup: ids never change for a live task; num_tasks shrinks on kill,
// so NEVER guard id against num_tasks. Scan the live list instead.
static task_t* task_lookup(int id){
    if(id<0||id>=MAX_TASKS) return 0;
    for(int i=0;i<num_tasks;i++)
        if(task_list[i]->id == id && task_list[i]->state != 99) return task_list[i];
    return 0;
}
// Deferred stack frees: if the victim is the currently-borrowed stack (i.e. a mouse IRQ
// preempted the victim and the kill runs on the victim's own stack, or the victim kills
// itself), unmapping it immediately would pull the floor out from under the running code
// (the impending IRQ iret resumes the victim briefly before the next PIT preempts it for
// good). Those pages are freed once the victim is guaranteed stopped: only entries
// older than 2 PIT ticks are drained (the scheduler skips tombstones, so after 2 ticks
// the victim cannot be running). Entries hold physical frames, so freeing is exact even
// though the vaddr may be reused. If the PIT ever stalls, draining stalls too and the
// pages merely leak - safe degradation, never a crash.
#define MAX_PENDING_FREE 4
static struct { uint32_t vbase; uint32_t p0; uint32_t p1; int id; uint32_t tick; } pending_free[MAX_PENDING_FREE];
static int pending_count = 0;
// An id is usable for a new task only if no LIST entry holds it (tombstones linger in the
// list until the scheduler purges them; reusing the id early would alias two entries).
static int task_id_in_list(int id){
    if(id<=0||id>=MAX_TASKS) return 0;
    for(int i=0;i<num_tasks;i++) if(task_list[i]->id == id) return 1;
    return 0;
}
// Must be called with interrupts masked (inside pushf/cli section).
static void task_process_pending(void){
    int w = 0;
    for(int i=0;i<pending_count;i++){
        // pit_ticks only advances in scheduler_tick; under cli the read is consistent.
        // pit_ticks is declared below; forward-declared here to keep helper order.
        extern int pit_get_ticks(void);
        if((uint32_t)(pit_get_ticks() - (int)pending_free[i].tick) < 2){
            pending_free[w++] = pending_free[i]; // too fresh: victim might still be on-CPU
            continue;
        }
        uint32_t vbase = pending_free[i].vbase;
        uint32_t ps[2]; ps[0]=pending_free[i].p0; ps[1]=pending_free[i].p1;
        int pid = pending_free[i].id;
        for(int j=0;j<2;j++){
            if(ps[j]){
                paging_unmap(vbase + j*4096);
                pmm_free_frame(ps[j]);
            }
        }
        s_puts("TASK: freed deferred stack id "); s_put_dec(pid); s_puts("\n");
    }
    pending_count = w;
}

int task_create_with_id(int want_id, void (*entry)(void), const char *name){
    if(num_tasks >= MAX_TASKS) return -1;
    // Drain deferred frees FIRST, before allocating/mapping anything: a deferred victim's
    // vaddr usually equals the vaddr we are about to map (same id reuse), so draining after
    // mapping would wipe the fresh PTEs and the new task would fault on its own stack
    // (triple fault: #PF delivery itself faults on the broken stack, resetting silently).
    // pushf/cli + popf preserves nesting (create runs in GUI and mouse-IRQ contexts).
    __asm__ volatile("pushf; cli" ::: "memory");
    task_process_pending();
    // Roles own permanent ids (Clicker=1, Notes=2) so UI tid mapping and tick slots stay valid
    // across kill/recreate churn. Honor want_id when free, else first free. "Free" means no
    // LIST entry holds the id (a lingering tombstone still occupies its slot until purged;
    // reusing early would alias two list entries to one tasks[] slot).
    // Picked under cli for a consistent task_list snapshot.
    int new_id = -1;
    if(want_id > 0 && want_id < MAX_TASKS && !task_id_in_list(want_id)) new_id = want_id;
    if(new_id==-1){
        for(int cand=1; cand<MAX_TASKS; cand++){
            if(!task_id_in_list(cand)){ new_id=cand; break; }
        }
    }
    __asm__ volatile("popf" ::: "memory");
    if(new_id==-1) return -1;
    // Avoid overlap with back buffer at 0x01000000 (16MB, 8.3M) and heap at 0x00400000
    // Use id-based vbase so killed stacks can be reused without overlap
    uint32_t vbase = 0x03000000 + new_id * TASK_STACK_SIZE;
    paging_ensure_range(vbase, TASK_STACK_SIZE);
    uint32_t paddrs[2] = {0,0};
    int pidx=0;
    for(int i=0;i<TASK_STACK_SIZE;i+=4096){
        uint32_t v = vbase + i;
        uint32_t p = pmm_alloc_frame();
        if(!p){ s_puts("TASK: out of frames\n"); return -1; }
        if(pidx<2) paddrs[pidx++]=p;
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
    // Slot == id by convention (Clicker=1, Notes=2); reused after kill.
    int slot = new_id;
    // Publish tail is the critical section: until task_list append + num_tasks++,
    // the PIT must not see a half-built slot. Zeroing above touched private pages only.
    // pushf/cli + popf preserves nesting (create is also called from mouse IRQ context).
    // NOTE: deferred drain lives at function top, NOT here: draining after mapping would
    // unmap the fresh pages when the id (hence vbase) is reused.
    uint64_t ccs0 = rdtsc();
    __asm__ volatile("pushf; cli" ::: "memory");
    tasks[slot].id = new_id;
    tasks[slot].esp = task_esp;
    tasks[slot].stack_base = (uint8_t*)vbase;
    tasks[slot].stack_top = stack_top;
    tasks[slot].stack_paddrs[0]=paddrs[0];
    tasks[slot].stack_paddrs[1]=paddrs[1];
    tasks[slot].state = TASK_READY;
    for(int i=0;i<16;i++) tasks[slot].name[i]=0;
    for(int i=0; name[i] && i<16;i++) tasks[slot].name[i]=name[i];
    task_list[num_tasks] = &tasks[slot];
    num_tasks++;
    __asm__ volatile("popf" ::: "memory");
    uint64_t ccs1 = rdtsc();
    s_puts("TASK: created "); s_puts(name); s_puts(" id "); s_put_dec(new_id); s_puts(" esp "); s_put_hex(task_esp);
    s_puts(" cli-section "); s_put_dec((uint32_t)(ccs1-ccs0)); s_puts(" cycles\n");
    return 0;
}

int task_create(void (*entry)(void), const char *name){
    return task_create_with_id(-1, entry, name);
}

task_t* task_current(void){ return task_list[current_task]; }
int task_count(void){ return num_tasks; }

static int pit_ticks = 0;
static int task_ticks[MAX_TASKS] = {0};
static int cpu_percent[MAX_TASKS] = {0};
static int mouse_irqs = 0;
volatile int g_report_ticks = 0;
// Drop tombstoned (state 99, killed) entries, except possibly the running one.
// Runs with PIT masked (scheduler_tick, or pushf/cli sections in kill/create), so the
// list cannot change under us. The running task is always task_list[current_task];
// purging it while it runs would break the pit-entry save invariant, so it is kept
// this tick (pick logic skips it below) and purged on a later tick once stopped.
static void task_purge_dead(void){
    task_t *cur = (current_task>=0 && current_task<num_tasks) ? task_list[current_task] : 0;
    int w = 0, newcur = 0;
    for(int r=0; r<num_tasks; r++){
        if(task_list[r]->state==99 && task_list[r]!=cur) continue; // drop dead non-running
        if(task_list[r]==cur) newcur = w;
        task_list[w++] = task_list[r];
    }
    num_tasks = w;
    current_task = newcur;
}
void scheduler_tick(void){
    pit_ticks++;
    // Scheduling is POSITION-based (positions into task_list; sched.s loads task_list[current_task]).
    // Ticks are accounted by permanent ID (task_ticks[task_list[next]->id]) so display/CPU% stay with the role.
    // GUI always lives at task_list[0] (id 0 is never killed) and gets every even phase (50%).
    // Odd phases round-robin the live non-GUI positions. task_kill only tombstones; dead
    // entries are purged here (never the running one) and skipped by the pick loop, so a
    // task killed while running stops at most one tick later and its struct is never
    // referenced after it stops. GUI (id 0) can never be tombstoned.
    // Trace pattern with 3 live tasks: pos 0,1,0,2,0,1,0,2... With 2 live: 0,1,0,1... GUI only: 0.
    task_purge_dead();
    static int phase = 0;
    static int rr = 1; // next non-GUI position to try
    int next = 0; // position; default GUI
    if(num_tasks > 1 && (phase % 2 == 1)){
        int n = num_tasks;
        if(rr < 1 || rr >= n) rr = 1;
        int tries = 0;
        while(tries < n){
            int cand = rr;
            rr++; if(rr >= n) rr = 1;
            tries++;
            if(task_list[cand]->state != 99){ next = cand; break; }
        }
    } else {
        next = 0;
    }
    phase++;
    task_ticks[task_list[next]->id]++;
    if(g_in_redraw){
        extern volatile int g_sched_during_redraw;
        g_sched_during_redraw++;
    }
    // Rolling CPU% every 100 ticks (1 sec at 100Hz) - not since-boot average
    // Math: total = gui + clicker + notes in last window, pct = ticks*100/total, total==0 => 0
    // Example: gui 50, clicker 25, notes 25 total 100 => 50%,25%,25%
    if(pit_ticks % 100 == 0){
        extern volatile int g_report_ticks;
        g_report_ticks = 1;
        int total = task_ticks[0] + task_ticks[1] + task_ticks[2];
        if(total==0){ cpu_percent[0]=0; cpu_percent[1]=0; cpu_percent[2]=0; }
        else { cpu_percent[0]=task_ticks[0]*100/total; cpu_percent[1]=task_ticks[1]*100/total; cpu_percent[2]=task_ticks[2]*100/total; }
        // Reset for next rolling window (1 sec) - keeps percentages recent, not since-boot
        // Note: we reset after reporting, so next window starts fresh
        // pit_reset_task_ticks will be called from main loop after reporting, but also need to keep cpu_percent until next window
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
void pit_get_cpu_percent(int *out_gui, int *out_a, int *out_b){
    if(out_gui) *out_gui = cpu_percent[0];
    if(out_a) *out_a = cpu_percent[1];
    if(out_b) *out_b = cpu_percent[2];
}
// All lookups skip tombstones (state 99): a killed task is gone for every consumer
// (UI buttons, icon reopen, re-kill) even before the scheduler purges its list entry.
static int task_lookup_pos(int id){
    if(id<0||id>=MAX_TASKS) return -1;
    for(int i=0;i<num_tasks;i++)
        if(task_list[i]->id == id && task_list[i]->state != 99) return i;
    return -1;
}
int task_exists(int id){
    return task_lookup_pos(id) != -1;
}
int task_find_by_name(const char *name){
    for(int i=0;i<num_tasks;i++){
        if(task_list[i]->state == 99) continue;
        int match=1;
        for(int j=0;name[j]||task_list[i]->name[j];j++) if(name[j]!=task_list[i]->name[j]){ match=0; break; }
        if(match) return task_list[i]->id;
    }
    return -1;
}
int task_kill_by_name(const char *name){
    int id = task_find_by_name(name);
    if(id==-1) return 0;
    return task_kill(id);
}
int task_kill(int id){
    if(id<=0) return 0; // never kill GUI (0)
    // Tombstone-only removal. Rationale (from a real triple-fault post-mortem): the victim
    // may still be the RUNNING task (kill issued from a mouse IRQ that preempted it, or
    // self-kill). Shifting task_list[] and rewriting current_task while it runs breaks the
    // pit-entry invariant (running task == task_list[current_task]), so the next PIT entry
    // saves the victim's ESP into the WRONG struct; later loading that ESP resumes into
    // freed/unmapped stack memory -> #PF -> double -> triple fault -> silent reset.
    // Instead: mark state 99 under cli (scheduler skips tombstones from the very next tick,
    // so the victim stops within 10ms), zero its tick counters, and defer-or-free its pages.
    // List surgery happens only in scheduler_tick/task_create purge paths, which never run
    // on a victim's stack and skip the running position. Serial logging stays OUTSIDE cli.
    uint64_t ccs0 = rdtsc();
    __asm__ volatile("pushf; cli" ::: "memory");
    task_process_pending();
    task_t *t = task_lookup(id); // skips tombstones: re-kill of dead task returns 0
    if(!t){ __asm__ volatile("popf" ::: "memory"); return 0; }
    // Borrowed-stack check: task_list[current_task] is the task whose stack we stand on
    // (scheduler invariant holds at kill entry: kills never rewrite current_task anymore).
    // If victim == borrowed, its pages must stay mapped until it stops (see pending rule).
    int is_borrowed = (current_task>=0 && current_task<num_tasks && task_list[current_task] == t);
    uint32_t paddrs[2]; paddrs[0]=t->stack_paddrs[0]; paddrs[1]=t->stack_paddrs[1];
    uint32_t vbase = (uint32_t)t->stack_base;
    char tname[16]; for(int i=0;i<16;i++) tname[i]=t->name[i];
    t->state = 99; // tombstone: scheduler skips it starting with the very next tick
    task_ticks[id]=0; cpu_percent[id]=0;
    int defer = is_borrowed;
    if(defer && pending_count >= MAX_PENDING_FREE){
        // Should not happen (MAX==MAX_TASKS and entries drain within 2 ticks); leak safely.
        defer = 2; // record-but-leak marker
    }
    if(defer==1){
        pending_free[pending_count].vbase = vbase;
        pending_free[pending_count].p0 = paddrs[0];
        pending_free[pending_count].p1 = paddrs[1];
        pending_free[pending_count].id = id;
        pending_free[pending_count].tick = (uint32_t)pit_ticks;
        pending_count++;
    }
    __asm__ volatile("popf" ::: "memory");
    uint64_t ccs1 = rdtsc();
    s_puts("TASK: kill id "); s_put_dec(id); s_puts(" "); s_puts(tname);
    if(defer) s_puts(" (stack free DEFERRED: victim may still be on-CPU)");
    else s_puts(" (was already stopped)");
    s_puts("\n");
    if(!defer){
        // Victim is not running anywhere: safe to unmap/free immediately.
        // Trace: vbase for id1 is 0x03002000 (2 pages), id2 is 0x03004000.
        for(int i=0;i<2;i++){
            if(paddrs[i]){
                uint32_t v = vbase + i*4096;
                paging_unmap(v);
                pmm_free_frame(paddrs[i]);
            }
        }
    } else if(defer==2){
        s_puts("TASK: WARNING pending full, leaking 2 stack pages (safe, no crash)\n");
    }
    s_puts("TASK: killed\n");
    s_puts(" cli-section "); s_put_dec((uint32_t)(ccs1-ccs0)); s_puts(" cycles\n");
    return 1;
}
void mouse_irq_count_inc(void){ mouse_irqs++; }
int mouse_get_irqs(void){ return mouse_irqs; }
void mouse_reset_irqs(void){ mouse_irqs=0; }

void task_yield(void){ __asm__ volatile("int $32"); }
void scheduler_start(void){ s_puts("TASK: scheduler ready\n"); }
