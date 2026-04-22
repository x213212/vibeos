#include "os.h"
#include "user.h"

#define CTRLDBG_PRINTF(...) do { } while (0)
extern void trap_vector();
extern void virtio_disk_isr();
extern void virtio_net_interrupt_handler();
extern void virtio_keyboard_isr();
extern void virtio_mouse_isr();
extern void virtio_input_poll(void);
extern reg_t app_exit_resume_pc(void);
extern reg_t app_exit_stack_top(void);
extern reg_t app_heap_alloc(reg_t size);
extern void app_mark_exited(void);
extern void terminal_app_stdout_putc(int win_id, char ch);
extern void terminal_app_stdout_puts(int win_id, const char *s);
extern char gui_key;
extern int os_jit_owner_active(int owner_win_id);
extern int os_jit_cancel_by_owner(int owner_win_id);
extern int os_jit_cancel_running_owner_from_trap(int owner_win_id);
extern void os_jit_cancel_trampoline(void);
extern void wake_terminal_worker_for_window(int win_idx);
extern int app_owner_win_id;
extern uint32_t APP_START, APP_END;
extern volatile int need_resched;
volatile int trap_skip_restore = 0;

static inline reg_t read_ra(void) {
    reg_t ra;
    asm volatile("mv %0, ra" : "=r"(ra));
    return ra;
}

void trap_init() {
    setup_mscratch_for_hart();
    w_pmpaddr0(((reg_t)(uintptr_t)app_l2_pt) >> 2);
    w_pmpaddr1((((reg_t)(uintptr_t)app_root_pt) + sizeof(uint32_t) * 1024) >> 2);
    w_pmpaddr2(((reg_t)(uintptr_t)APP_START) >> 2);
    w_pmpaddr3(((reg_t)(uintptr_t)APP_END) >> 2);
    w_pmpcfg0(0x0f000b00);
    w_mtvec((reg_t)trap_vector);
    w_mstatus(r_mstatus() | MSTATUS_MIE); 
}

void external_handler() {
    int irq = plic_claim();
    if (irq == 1) { // Disk
        virtio_disk_isr();
    } else if (irq == 2) { // Net
        virtio_net_interrupt_handler();
    } else if (irq == 3) { // Keyboard (Slot 2)
        virtio_keyboard_isr();
    } else if (irq == 4) { // Mouse (Slot 3)
        virtio_mouse_isr();
    } else if (irq == 10) { // UART
        lib_isr();
    }
    if (irq) plic_complete(irq);
}

static void external_drain_pending(void) {
    while (1) {
        int irq = plic_claim();
        if (!irq) break;
        if (irq == 1) {
            virtio_disk_isr();
        } else if (irq == 2) {
            virtio_net_interrupt_handler();
        } else if (irq == 3) {
            virtio_keyboard_isr();
        } else if (irq == 4) {
            virtio_mouse_isr();
        } else if (irq == 10) {
            lib_isr();
        }
        plic_complete(irq);
    }
}

reg_t trap_handler(reg_t epc, reg_t cause, reg_t frame) {
    reg_t return_pc = epc;
    int advance_pc = 1;
    reg_t cause_code = cause & 0xfff;
    if (cause & 0x80000000) {
        if (cause_code == 7) {
            w_mie(r_mie() & ~(1 << 7));
            timer_handler();
            need_resched = 1;
            external_drain_pending();
            virtio_keyboard_isr();
            virtio_mouse_isr();
            virtio_input_poll();
            if (gui_key == 3 &&
                active_win_idx >= 0 && active_win_idx < MAX_WINDOWS &&
                wins[active_win_idx].active &&
                wins[active_win_idx].kind == WINDOW_KIND_TERMINAL) {
                struct Window *w = &wins[active_win_idx];
                if (w->submit_locked || w->executing_cmd || w->waiting_wget || os_jit_owner_active(w->id)) {
                    int killed = 0;
                    CTRLDBG_PRINTF("[CTRLDBG] trap win=%d exec=%d wget=%d jit=%d\n",
                                   w->id, w->executing_cmd, w->waiting_wget, os_jit_owner_active(w->id));
                    w->cancel_requested = 1;
                    killed = os_jit_cancel_running_owner_from_trap(w->id);
                    CTRLDBG_PRINTF("[CTRLDBG] trap win=%d killed=%d\n", w->id, killed);
                    if (killed > 0) {
                        w->mailbox = 1;
                        wake_terminal_worker_for_window(w->id);
                        gui_key = 0;
                        return_pc = (reg_t)(uintptr_t)os_jit_cancel_trampoline;
                        advance_pc = 0;
                    }
                    gui_key = 0;
                }
            }
            w_mie(r_mie() | MIE_MTIE);
        } else if (cause_code == 11) {
            external_handler();
        }
    } else if (cause_code == 8 || cause_code == 11) {
        reg_t *regs = (reg_t *)frame;
        reg_t sysno = regs[16];
        if (sysno == 1) {
            if (app_owner_win_id >= 0) terminal_app_stdout_putc(app_owner_win_id, (char)regs[9]);
            else lib_putc((char)regs[9]);
        } else if (sysno == 2) {
            if (app_owner_win_id >= 0) terminal_app_stdout_puts(app_owner_win_id, (char *)regs[9]);
            else lib_puts((char *)regs[9]);
        } else if (sysno == 3) {
            regs[9] = app_heap_alloc(regs[9]);
        } else if (sysno == 4) {
            regs[9] = 0;
        } else if (sysno == 5) {
            regs[9] = (reg_t)get_millisecond_timer();
        } else if (sysno == 6) {
            need_resched = 1;
        } else if (sysno == 7) {
            regs[9] = (reg_t)appfs_open((const char *)(uintptr_t)regs[9], (int)regs[10]);
        } else if (sysno == 8) {
            regs[9] = (reg_t)appfs_read((int)regs[9], (void *)(uintptr_t)regs[10], (size_t)regs[11]);
        } else if (sysno == 9) {
            regs[9] = (reg_t)appfs_write((int)regs[9], (const void *)(uintptr_t)regs[10], (size_t)regs[11]);
        } else if (sysno == 10) {
            extern int loaded_app_exit_code;
            loaded_app_exit_code = (int)regs[9];
            regs[1] = app_exit_stack_top();
            w_satp(0);
            sfence_vma();
            w_mstatus((r_mstatus() & ~MSTATUS_MPP_MASK) | MSTATUS_MPP_M);
            return_pc = app_exit_resume_pc();
            advance_pc = 0;
        } else if (sysno == 11) {
            regs[9] = (reg_t)appfs_close((int)regs[9]);
        } else {
            panic("bad ecall");
        }
        if (advance_pc) {
            return_pc += 4;
        }
    } else {
        lib_printf("trap fault cause=%lu epc=%lx mtval=%lx\n",
                   (unsigned long)cause_code,
                   (unsigned long)epc,
                   (unsigned long)r_mtval());
        panic("trap fault");
    }
    return return_pc;
}
