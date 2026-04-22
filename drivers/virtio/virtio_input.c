#include "virtio.h"
#include "os.h"
#include "vga.h"

#define CTRLDBG_PRINTF(...) do { } while (0)

#define INPUT_QUEUE_SIZE 64
#define INPUT_EVENT_QUEUE_SIZE 512

int gui_mx=WIDTH/2, gui_my=HEIGHT/2, gui_clicked=0, gui_click_pending=0, gui_wheel=0;
int gui_right_clicked=0, gui_right_click_pending=0;
int gui_shortcut_new_task = 0;
int gui_shortcut_close_task = 0;
int gui_shortcut_switch_task = 0; 
int gui_ctrl_pressed = 0;
char gui_key=0;
int gbemu_btn_a = 0;
int gbemu_btn_b = 0;
int gbemu_btn_start = 0;
int gbemu_btn_select = 0;
int gbemu_btn_up = 0;
int gbemu_btn_down = 0;
int gbemu_btn_left = 0;
int gbemu_btn_right = 0;
extern int os_debug;

struct raw_input_event {
    uint16 device;
    uint16 type;
    uint16 code;
    uint32 value;
};

static volatile uint16 input_evt_head = 0;
static volatile uint16 input_evt_tail = 0;
static volatile int input_poll_busy = 0;
static struct raw_input_event input_events[INPUT_EVENT_QUEUE_SIZE];

static void enqueue_input_event(uint16 device, uint16 type, uint16 code, uint32 value) {
    uint16 next = (input_evt_tail + 1) % INPUT_EVENT_QUEUE_SIZE;
    if (next == input_evt_head) {
        input_evt_head = (input_evt_head + 1) % INPUT_EVENT_QUEUE_SIZE;
    }
    input_events[input_evt_tail].device = device;
    input_events[input_evt_tail].type = type;
    input_events[input_evt_tail].code = code;
    input_events[input_evt_tail].value = value;
    input_evt_tail = next;
}

struct input_avail {
    uint16 flags; uint16 idx; uint16 ring[INPUT_QUEUE_SIZE];
};
struct input_used_elem { uint32 id; uint32 len; };
struct input_used {
    uint16 flags; uint16 idx; struct input_used_elem ring[INPUT_QUEUE_SIZE];
};

struct keyboard {
    char pages[2 * PGSIZE];
    virtq_desc_t *desc;
    struct input_avail *avail;
    struct input_used *used;
    struct { uint16 type, code; uint32 value; } events[INPUT_QUEUE_SIZE];
    uint16 queue_size;
    uint16 used_idx;
} __attribute__((aligned(4096))) keyboard;

void virtio_keyboard_init() {
    volatile uint32 *r = (uint32*)VIRTIO_KBD_BASE; if(*r != 0x74726976) return;
    uint16 qsize = (uint16)r[VIRTIO_MMIO_QUEUE_NUM_MAX / 4];
    if (qsize == 0) return;
    if (qsize > INPUT_QUEUE_SIZE) qsize = INPUT_QUEUE_SIZE;
    keyboard.queue_size = qsize;
    memset(keyboard.pages, 0, sizeof(keyboard.pages));
    r[28]=0x0F; r[10]=PGSIZE; r[14]=qsize; r[16]=((uint32)keyboard.pages)>>12;
    keyboard.desc=(virtq_desc_t*)keyboard.pages;
    keyboard.avail=(struct input_avail*)(keyboard.pages + INPUT_QUEUE_SIZE * sizeof(virtq_desc_t));
    keyboard.used=(struct input_used*)(keyboard.pages+PGSIZE);
    for(int i=0;i<qsize;i++){ keyboard.desc[i].addr=(uint64)(uint32)&keyboard.events[i]; keyboard.desc[i].len=8; keyboard.desc[i].flags=2; keyboard.avail->ring[i]=i; }
    keyboard.avail->idx=qsize; r[28]|=4; if (os_debug) lib_puts("KBD Ready\n");
}

void virtio_keyboard_isr() {
    if(!keyboard.used || keyboard.queue_size == 0) return;
    uint16 cur = keyboard.used->idx;
    while(keyboard.used_idx != cur) {
        int id = keyboard.used->ring[keyboard.used_idx % keyboard.queue_size].id;
        if (id < 0 || id >= keyboard.queue_size) break;
        enqueue_input_event(0, keyboard.events[id].type, keyboard.events[id].code, keyboard.events[id].value);
        keyboard.avail->ring[keyboard.avail->idx % keyboard.queue_size]=id; keyboard.avail->idx++; keyboard.used_idx++;
    }
    *(volatile uint32*)(VIRTIO_KBD_BASE + 0x64) = *(volatile uint32*)(VIRTIO_KBD_BASE + 0x60) & 3;
}

struct mouse {
    char pages[2 * PGSIZE];
    virtq_desc_t *desc;
    struct input_avail *avail;
    struct input_used *used;
    struct { uint16 type, code; uint32 value; } events[INPUT_QUEUE_SIZE];
    uint16 queue_size;
    uint16 used_idx;
} __attribute__((aligned(4096))) mouse;
void virtio_mouse_init() {
    volatile uint32 *r = (uint32*)VIRTIO_MOUSE_BASE; if(*r != 0x74726976) return;
    uint16 qsize = (uint16)r[VIRTIO_MMIO_QUEUE_NUM_MAX / 4];
    if (qsize == 0) return;
    if (qsize > INPUT_QUEUE_SIZE) qsize = INPUT_QUEUE_SIZE;
    mouse.queue_size = qsize;
    memset(mouse.pages, 0, sizeof(mouse.pages));
    r[28]=0x0F; r[10]=PGSIZE; r[14]=qsize; r[16]=((uint32)mouse.pages)>>12;
    mouse.desc=(virtq_desc_t*)mouse.pages;
    mouse.avail=(struct input_avail*)(mouse.pages + INPUT_QUEUE_SIZE * sizeof(virtq_desc_t));
    mouse.used=(struct input_used*)(mouse.pages+PGSIZE);
    for(int i=0;i<qsize;i++){ mouse.desc[i].addr=(uint64)(uint32)&mouse.events[i]; mouse.desc[i].len=8; mouse.desc[i].flags=2; mouse.avail->ring[i]=i; }
    mouse.avail->idx=qsize; r[28]|=4; if (os_debug) lib_puts("Mouse Ready\n");
}
void virtio_mouse_isr() {
    if(!mouse.used || mouse.queue_size == 0) return;
    uint16 cur = mouse.used->idx;
    while(mouse.used_idx != cur) {
        int id = mouse.used->ring[mouse.used_idx % mouse.queue_size].id;
        if (id < 0 || id >= mouse.queue_size) break;
        enqueue_input_event(1, mouse.events[id].type, mouse.events[id].code, mouse.events[id].value);
        mouse.avail->ring[mouse.avail->idx % mouse.queue_size] = id;
        mouse.avail->idx++; mouse.used_idx++;
    }
    *(volatile uint32*)(VIRTIO_MOUSE_BASE + 0x64) = *(volatile uint32*)(VIRTIO_MOUSE_BASE + 0x60) & 3;
}

void virtio_input_poll(void) {
    static int caps_lock = 0, shift_pressed = 0, ctrl_l_pressed = 0, ctrl_r_pressed = 0;
    if (input_poll_busy) return;
    input_poll_busy = 1;
    // 使用舊版穩定的 kmap 和 smap
    static char kmap[128] = {
        [1]=27, [2]='1', [3]='2', [4]='3', [5]='4', [6]='5', [7]='6', [8]='7', [9]='8', [10]='9', [11]='0', [0x0C]='-', [0x0D]='=', [0x0E]=8, [0x0F]='\t',
        [0x10]='q', [0x11]='w', [0x12]='e', [0x13]='r', [0x14]='t', [0x15]='y', [0x16]='u', [0x17]='i', [0x18]='o', [0x19]='p', [0x1A]='[', [0x1B]=']', [0x1C]=10,
        [0x1E]='a', [0x1F]='s', [0x20]='d', [0x21]='f', [0x22]='g', [0x23]='h', [0x24]='j', [0x25]='k', [0x26]='l', [0x27]=';', [0x28]='\'', [0x29]='`',
        [0x2B]='\\', [0x2C]='z', [0x2D]='x', [0x2E]='c', [0x2F]='v', [0x30]='b', [0x31]='n', [0x32]='m', [0x33]=',', [0x34]='.', [0x35]='/', [0x39]=' ',
        [0x47]='7', [0x48]='8', [0x49]='9', [0x4A]='-', [0x4B]='4', [0x4C]='5', [0x4D]='6', [0x4E]='+', [0x4F]='1', [0x50]='2', [0x51]='3', [0x52]='0', [0x53]='.', [0x60]=10, [0x62]='/'
    };
    static char smap[128] = {
        [2]='!', [3]='@', [4]='#', [5]='$', [6]='%', [7]='^', [8]='&', [9]='*', [10]='(', [11]=')', [0x0C]='_', [0x0D]='+',
        [0x1A]='{', [0x1B]='}', [0x27]=':', [0x28]='"', [0x29]='~', [0x2B]='|', [0x33]='<', [0x34]='>', [0x35]='?'
    };

    while (input_evt_head != input_evt_tail) {
        struct raw_input_event ev = input_events[input_evt_head];
        input_evt_head = (input_evt_head + 1) % INPUT_EVENT_QUEUE_SIZE;

        if (ev.device == 0) { // Keyboard
            if (ev.type != 1) continue;

            // 1. 處理狀態鍵 (Shift, Ctrl) - 這部分必須先做
            if (ev.code == 0x2A || ev.code == 0x36) { shift_pressed = (ev.value == 1); }
            else if (ev.code == 29) {
                ctrl_l_pressed = (ev.value == 1);
                gui_ctrl_pressed = (ctrl_l_pressed || ctrl_r_pressed);
                CTRLDBG_PRINTF("[CTRLDBG] input lctrl value=%u pressed=%d any=%d\n",
                               ev.value, ctrl_l_pressed, gui_ctrl_pressed);
            }
            else if (ev.code == 97) {
                ctrl_r_pressed = (ev.value == 1);
                gui_ctrl_pressed = (ctrl_l_pressed || ctrl_r_pressed);
                CTRLDBG_PRINTF("[CTRLDBG] input rctrl value=%u pressed=%d any=%d\n",
                               ev.value, ctrl_r_pressed, gui_ctrl_pressed);
            }
            
            // 2. 處理按下 (Key Down)
            if (ev.value == 1) {
                if (ev.code == 46) {
                    CTRLDBG_PRINTF("[CTRLDBG] input raw-c ctrl_l=%d ctrl_r=%d any=%d\n",
                                   ctrl_l_pressed, ctrl_r_pressed, (ctrl_l_pressed || ctrl_r_pressed));
                }
                // 特殊快捷鍵
                if ((ctrl_l_pressed || ctrl_r_pressed) && ev.code == 15) gui_shortcut_switch_task = 1;
                else if ((ctrl_l_pressed || ctrl_r_pressed) && ev.code == 20) gui_shortcut_new_task = 1;
                else if ((ctrl_l_pressed || ctrl_r_pressed) && ev.code == 16) gui_shortcut_close_task = 1;
                else if ((ctrl_l_pressed || ctrl_r_pressed) && ev.code == 46) {
                    gui_key = 3;
                    CTRLDBG_PRINTF("[CTRLDBG] input ctrl-c gui_key=%d\n", gui_key);
                }
                else if (ev.code == 0x3A) caps_lock = !caps_lock;
                // 方向鍵與特殊鍵 (恢復 Vim 支援)
                else if (ev.code == 103) gui_key = 0x10; // Up
                else if (ev.code == 108) gui_key = 0x11; // Down
                else if (ev.code == 105) gui_key = 0x12; // Left
                else if (ev.code == 106) gui_key = 0x13; // Right
                else if (ev.code == 111) gui_key = 0x14; // Delete
                else if (ev.code == 102) gui_key = 0x15; // Home
                else if (ev.code == 107) gui_key = 0x16; // End
                else if (ev.code == 104) gui_key = 0x17; // PageUp
                else if (ev.code == 109) gui_key = 0x18; // PageDown
                else if (ev.code == 110) gui_key = 0x19; // Insert
                else if (ev.code < 128) {
                    char k = kmap[ev.code];
                    if (shift_pressed && smap[ev.code]) gui_key = smap[ev.code];
                    else if ((k >= 'a' && k <= 'z') && (caps_lock ^ shift_pressed)) gui_key = k - 'a' + 'A';
                    else gui_key = k;
                }

                // 3. 同時更新 GameBoy 狀態 (不使用 else if，確保與 gui_key 並行)
                if (ev.code == 0x2C) gbemu_btn_a = 1;
                if (ev.code == 0x2D) gbemu_btn_b = 1;
                if (ev.code == 0x1C || ev.code == 0x60) gbemu_btn_start = 1;
                if (ev.code == 0x0F) gbemu_btn_select = 1;
                if (ev.code == 103) gbemu_btn_up = 1;
                if (ev.code == 108) gbemu_btn_down = 1;
                if (ev.code == 105) gbemu_btn_left = 1;
                if (ev.code == 106) gbemu_btn_right = 1;
            } 
            // 4. 處理放開 (Key Up)
            else if (ev.value == 0) {
                if (ev.code == 0x2C) gbemu_btn_a = 0;
                if (ev.code == 0x2D) gbemu_btn_b = 0;
                if (ev.code == 0x1C || ev.code == 0x60) gbemu_btn_start = 0;
                if (ev.code == 0x0F) gbemu_btn_select = 0;
                if (ev.code == 103) gbemu_btn_up = 0;
                if (ev.code == 108) gbemu_btn_down = 0;
                if (ev.code == 105) gbemu_btn_left = 0;
                if (ev.code == 106) gbemu_btn_right = 0;
            }
        } else if (ev.device == 1) { // Mouse
            if (ev.type == 3) {
                if (ev.code == 0) gui_mx = (ev.value * WIDTH) / 32768;
                if (ev.code == 1) gui_my = (ev.value * HEIGHT) / 32768;
            } else if (ev.type == 2 && ev.code == 8) {
                gui_wheel = (int)ev.value;
            } else if (ev.type == 1 && ev.code == 0x110) {
                gui_clicked = (ev.value == 1);
                if (gui_clicked) gui_click_pending = 1;
            } else if (ev.type == 1 && ev.code == 0x111) {
                gui_right_clicked = (ev.value == 1);
                if (gui_right_clicked) gui_right_click_pending = 1;
            }
        }
    }
    input_poll_busy = 0;
}
