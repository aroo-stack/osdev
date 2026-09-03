#include "heap.h"
#include "paging.h"
#include "pmm.h"
#include <stdint.h>
#include <stddef.h>

#define HEAP_START 0x00400000u // past 4MB identity map
#define HEAP_SIZE  0x00100000u // 1MB (256 pages)
#define HEAP_END   (HEAP_START + HEAP_SIZE)

struct heap_header {
    uint32_t size; // usable payload size
    uint8_t free;
    uint8_t _pad[3];
    struct heap_header *next;
};

static struct heap_header *heap_first = 0;

static inline void outb(uint16_t port, uint8_t v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(port));}
static inline uint8_t inb(uint16_t port){ uint8_t r; __asm__ volatile("inb %1,%0":"=a"(r):"Nd"(port)); return r;}
static int tx_empty(){ return inb(0x3F8+5)&0x20; }
static void s_putc(char c){ while(!tx_empty()); outb(0x3F8,c); }
static void s_puts(const char*s){ for(size_t i=0;s[i];i++) s_putc(s[i]); }
static void s_put_hex32(uint32_t n){ s_puts("0x"); for(int i=28;i>=0;i-=4){ uint8_t v=(n>>i)&0xF; s_putc(v<10?'0'+v:'A'+v-10); } }
static void s_put_dec(uint32_t n){ char b[11]; int i=0; if(n==0){s_putc('0');return;} while(n){b[i++]='0'+n%10; n/=10;} while(i--) s_putc(b[i]); }

void heap_init(void){
    s_puts("HEAP: init virtual "); s_put_hex32(HEAP_START); s_puts(" - "); s_put_hex32(HEAP_END); s_puts(" ("); s_put_dec(HEAP_SIZE/1024); s_puts(" KB)\n");
    // map each page of heap virtual to a fresh physical frame
    for(uint32_t v=HEAP_START; v < HEAP_END; v+=0x1000){
        uint32_t p = pmm_alloc_frame();
        if(p==0){
            s_puts("HEAP: out of frames during map!\n");
            break;
        }
        paging_map(v, p, 0x03); // RW, present
    }
    // zero heap area (now mapped)
    for(uint32_t v=HEAP_START; v < HEAP_END; v+=4){
        *(volatile uint32_t*)v = 0;
    }
    heap_first = (struct heap_header*)HEAP_START;
    heap_first->size = HEAP_SIZE - sizeof(struct heap_header);
    heap_first->free = 1;
    heap_first->next = 0;
    s_puts("HEAP: first block at "); s_put_hex32((uint32_t)heap_first); s_puts(" size "); s_put_dec(heap_first->size); s_puts("\n");
}

static uint32_t align8(uint32_t n){ return (n+7)&~7u; }

void* kmalloc(size_t size){
    if(size==0) return 0;
    size = align8(size);
    struct heap_header *cur = heap_first;
    while(cur){
        if(cur->free && cur->size >= size){
            // split if large enough to hold header + 8
            if(cur->size >= size + sizeof(struct heap_header) + 8){
                uint32_t split_at = (uint32_t)cur + sizeof(struct heap_header) + size;
                struct heap_header *nxt = (struct heap_header*)split_at;
                nxt->size = cur->size - size - sizeof(struct heap_header);
                nxt->free = 1;
                nxt->next = cur->next;
                cur->size = size;
                cur->next = nxt;
            }
            cur->free = 0;
            return (void*)((uint32_t)cur + sizeof(struct heap_header));
        }
        cur = cur->next;
    }
    return 0;
}

void kfree(void *ptr){
    if(!ptr) return;
    struct heap_header *hdr = (struct heap_header*)((uint32_t)ptr - sizeof(struct heap_header));
    hdr->free = 1;
    // coalesce with next
    if(hdr->next && hdr->next->free){
        hdr->size += sizeof(struct heap_header) + hdr->next->size;
        hdr->next = hdr->next->next;
    }
    // coalesce with previous - need to find prev
    struct heap_header *cur = heap_first;
    struct heap_header *prev = 0;
    while(cur && cur != hdr){
        prev = cur;
        cur = cur->next;
    }
    if(prev && prev->free){
        prev->size += sizeof(struct heap_header) + hdr->size;
        prev->next = hdr->next;
    }
}

void heap_print_stats(void){
    int blocks=0, free_blocks=0;
    uint32_t free_bytes=0;
    struct heap_header *c=heap_first;
    while(c){ blocks++; if(c->free){ free_blocks++; free_bytes+=c->size; } c=c->next; }
    s_puts("HEAP stats: blocks "); s_put_dec(blocks); s_puts(" free "); s_put_dec(free_blocks); s_puts(" free_bytes "); s_put_dec(free_bytes); s_puts("\n");
}
