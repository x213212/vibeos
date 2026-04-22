#include "os.h"
#include "vga.h"
#include "mbedtls_port.h"

extern void trap_init(void);
extern void virtio_disk_init(void);
extern void virtio_net_init(void);
extern void virtio_keyboard_init(void);
extern void virtio_mouse_init(void);
extern void draw_rect_fill(int x, int y, int w, int h, int color);
extern void draw_text(int x, int y, const char *s, int color);
extern void vga_update(void);
volatile int need_resched = 0;
int os_debug = 0;

void os_kernel()
{
	task_os();
}

void disk_read() {}

void os_start()
{
	uart_init();
	page_init();
	vga_init();
	draw_rect_fill(0, 0, WIDTH, HEIGHT, 0);
	draw_text(16, 16, "Booting...", 15);
	vga_update();
	if (os_debug) lib_puts("Hacking OS Initializing...\n");
	if (os_debug) lib_printf("[BOOT] after uart/page/vga\n");
	if (os_debug) lib_printf("[BOOT] before mbedtls_os_init\n");
	mbedtls_os_init();
	if (os_debug) lib_printf("[BOOT] after mbedtls_os_init\n");
	if (os_debug) lib_printf("[BOOT] before user_init\n");
	user_init();
	if (os_debug) lib_printf("[BOOT] after user_init\n");
	if (os_debug) lib_printf("[BOOT] before trap_init\n");
	trap_init();
	if (os_debug) lib_printf("[BOOT] after trap_init\n");
	if (os_debug) lib_printf("[BOOT] before plic_init\n");
	plic_init();
	if (os_debug) lib_printf("[BOOT] after plic_init\n");
	if (os_debug) lib_printf("[BOOT] before virtio_disk_init\n");
	virtio_disk_init();
	if (os_debug) lib_printf("[BOOT] after virtio_disk_init\n");
	if (os_debug) lib_printf("[BOOT] before virtio_net_init\n");
	virtio_net_init();
	if (os_debug) lib_printf("[BOOT] after virtio_net_init\n");
	if (os_debug) lib_printf("[BOOT] before virtio_keyboard_init\n");
	virtio_keyboard_init();
	if (os_debug) lib_printf("[BOOT] after virtio_keyboard_init\n");
	if (os_debug) lib_printf("[BOOT] before virtio_mouse_init\n");
	virtio_mouse_init();
	if (os_debug) lib_printf("[BOOT] after virtio_mouse_init\n");
	if (os_debug) lib_printf("[BOOT] before timer_init\n");
	timer_init();
	if (os_debug) lib_printf("[BOOT] after timer_init\n");
	if (os_debug) lib_printf("[BOOT] before page_test\n");
	page_test();
	if (os_debug) lib_printf("[BOOT] after page_test\n");
}

int os_main(void) {
    if (os_debug) lib_printf("[BOOT] os_main enter\n");
    os_start();
    if (os_debug) lib_printf("[BOOT] os_main after start\n");
    while (1) {
        int current_task = task_next();
        if (os_debug) lib_printf("[BOOT] task_next=%d\n", current_task);
        if (current_task < 0) continue;
        if (os_debug) lib_printf("Go Task %d\n", current_task);
        if (os_debug) lib_printf("[BOOT] task_go=%d\n", current_task);
        task_go(current_task);
    }
}
