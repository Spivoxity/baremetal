// microbian.c
// Copyright (c) 2018 J. M. Spivey

#include "microbian.h"
#include "lib.h"
#include "hardware.h"
#include <string.h>

/* PROCESS DESCRIPTORS */

struct proc {
    int p_pid;                  /* Process ID (equal to index) */
    char p_name[16];            /* Name for debugging */
    unsigned p_state;           /* SENDING, RECEIVING, etc. */
    unsigned *p_sp;             /* Saved stack pointer */
    void *p_stack;              /* Stack area */
    unsigned p_stksize;         /* Stack size (bytes) */
    int p_priority;             /* Priority: 0 is highest */
    
    struct proc *p_waiting;     /* Processes waiting to send */
    int p_pending;              /* Whether HARDWARE message pending */
    int p_msgtype;              /* Message type to send or recieve */
    message *p_message;         /* Pointer to message buffer */
    struct proc *p_next;        /* Next process in ready or send queue */
};

/* Possible p_state values */
#define DEAD 0
#define ACTIVE 1
#define SENDING 2
#define RECEIVING 3
#define SENDREC 4
#define IDLING 5


/* STORAGE ALLOCATION */

/* Stack space for processes is allocated from the low end of the
space between the static data and the main stack.  Process
descriptors are allocated from the opposite end of the space; this
is deliberate to reduce the likelihood that a process overrunning
its stack space will destroy its own descriptor or that of its
neighbour. */

extern unsigned char __stack_limit[], __end[];

static unsigned char *__break = __end;
static unsigned char *__break2 = __stack_limit;

#define ROUNDUP(x, n)  (((x)+(n)-1) & ~((n)-1))

static void *sbrk(int inc) {
    if (inc > __break2 - __break)
        panic("Phos is out of memory");
    void *result = __break;
    __break += inc;
    return result;
}

static struct proc *new_proc(void) {
    if (__break2 - __break < sizeof(struct proc))
        panic("No space for process");
    __break2 -= sizeof(struct proc);
    return (struct proc *) __break2;
}


/* PROCESS TABLE */

#define NPROCS 32

static struct proc *os_ptable[NPROCS];
static unsigned os_nprocs = 0;

static struct proc *os_current;
static struct proc *idle_proc;

#define BLANK 0xdeadbeef        /* Filler for initial stack */

static void kprintf_setup(void);
static void kprintf_internal(char *fmt, ...);

/* microbian_dump -- display process states */
static void microbian_dump(void) {
    char buf1[16], buf2[16];

    static const char *status[] = {
        "[DEAD]   ",
        "[ACTIVE] ",
        "[SENDING]",
        "[RCVING] ",
        "[SENDREC]",
        "[IDLING] "
    };

    kprintf_setup();
    kprintf_internal("\r\nPROCESS DUMP\r\n");

    // Our version of printf is a bit feeble, so the following is
    // rather painful.

    for (int pid = 0; pid < os_nprocs; pid++) {
        struct proc *p = os_ptable[pid];
        unsigned *z = (unsigned *) p->p_stack;
        while (*z == BLANK) z++;
        unsigned free = (char *) z - (char *) p->p_stack;

        if (pid < 10)
            sprintf(buf1, " %d", pid);
        else
            sprintf(buf1, "%d", pid);

        sprintf(buf2, "%u/%u", p->p_stksize-free, p->p_stksize);
        int w = strlen(buf2);
        if (w < 9) {
            memset(buf2+w, ' ', 9-w);
            buf2[9] = '\0';
        }

        kprintf_internal("%s: %s %x stk=%s %s\r\n",
                         buf1, status[p->p_state],
                         (unsigned) p->p_stack,
                         buf2, p->p_name);
    }
}


/* PROCESS QUEUES */

/* os_readyq -- one queue for each priority */
static struct queue {
    struct proc *q_head, *q_tail;
} os_readyq[3];

/* make_ready -- add process to end of appropriate queue */
static inline void make_ready(struct proc *p, int prio) {
    if (prio == P_IDLE) return;

    p->p_state = ACTIVE;
    p->p_next = NULL;

    struct queue *q = &os_readyq[prio];
    if (q->q_head == NULL)
        q->q_head = p;
    else
        q->q_tail->p_next = p;
    q->q_tail = p;
}

/* choose_proc -- the current process is blocked: pick a new one */
static inline void choose_proc(void) {
    for (int p = 0; p < 3; p++) {
        struct queue *q = &os_readyq[p];
        if (q->q_head != NULL) {
            os_current = q->q_head;
            q->q_head = os_current->p_next;
            return;
        }
    }

    os_current = idle_proc;
}


/* SEND AND RECEIVE */

/* These versions of send and receive are invoked indirectly from user
   processes via the system calls send() and receive(). */

/* accept -- test if a process is waiting for a message of given type */
static inline int accept(struct proc *pdest, int type) {
    return (pdest->p_state == RECEIVING
            && (pdest->p_msgtype == ANY || pdest->p_msgtype == type));
}

/* set_state -- set process state for send or receive */
static inline void set_state(struct proc *p, int state,
                             int type, message *msg) {
    p->p_state = state;
    p->p_msgtype = type;
    p->p_message = msg;
}

/* deliver -- copy a message and fill in standard fields */
static inline void deliver(message *buf, int src, int type, message *msg) {
    if (buf) {
        if (msg) *buf = *msg;
        buf->m_sender = src;
        buf->m_type = type;
    }
}

/* enqueue -- add current process to a receiver's queue */
static inline void enqueue(struct proc *pdest) {
    os_current->p_next = NULL;
    if (pdest->p_waiting == NULL)
        pdest->p_waiting = os_current;
    else {
        struct proc *r = pdest->p_waiting;
        while (r->p_next != NULL)
            r = r->p_next;
        r->p_next = os_current;
    }
}

/* mini_send -- send a message */
static void mini_send(int dest, int type, message *msg) {
    int src = os_current->p_pid;
    struct proc *pdest = os_ptable[dest];

    if (dest < 0 || dest >= os_nprocs || pdest->p_state == DEAD)
        panic("Sending to a non-existent process %d", dest);

    if (accept(pdest, type)) {
        // Receiver is waiting for us
        deliver(pdest->p_message, src, type, msg);
        make_ready(pdest, pdest->p_priority);
    } else {
        // Sender must wait by joining the receiver's queue
        set_state(os_current, SENDING, type, msg);
        enqueue(pdest);
        choose_proc();
    }
}

/* mini_receive -- receive a message */
static void mini_receive(int type, message *msg) {
    // First see if an interrupt is pending
    if (os_current->p_pending && (type == ANY || type == INTERRUPT)) {
        os_current->p_pending = 0;
        deliver(msg, HARDWARE, INTERRUPT, NULL);
        return;
    }

    if (type != INTERRUPT) {
        // Now look for a process waiting to send an accepable message
        struct proc *psrc, *prev = NULL;
        
        for (psrc = os_current->p_waiting; psrc != NULL;
             psrc = psrc->p_next) {            
            if (type == ANY || psrc->p_msgtype == type) {
                if (prev == NULL)
                    os_current->p_waiting = psrc->p_next;
                else
                    os_current->p_next = psrc->p_next;

                deliver(msg, psrc->p_pid, psrc->p_msgtype, psrc->p_message);
                if (psrc->p_state == SENDING)
                    make_ready(psrc, psrc->p_priority);
                else {
                    // After sending, a SENDREC process waits for a reply.
                    assert(psrc->p_state == SENDREC);
                    set_state(psrc, RECEIVING, REPLY, msg);
                }
                return;
            }

            prev = psrc;
        }
    }

    // No luck: we must wait.
    set_state(os_current, RECEIVING, type, msg);
    choose_proc();
}    

static void mini_sendrec(int dest, int type, message *msg) {
    int src = os_current->p_pid;
    struct proc *pdest = os_ptable[dest];

    if (dest < 0 || dest >= os_nprocs || pdest->p_state == DEAD)
        panic("Sending to a non-existent process %d", dest);

    if (accept(pdest, type)) {
        // Receiver is waiting for us
        deliver(pdest->p_message, src, type, msg);
        make_ready(pdest, pdest->p_priority);

        // Now we must wait for a reply
        set_state(os_current, RECEIVING, REPLY, msg);
    } else {
        // Sender must wait by joining the receiver's queue
        set_state(os_current, SENDREC, type, msg);
        enqueue(pdest);
    }

    choose_proc();
}    


/* INTERRUPT HANDLING */

/* Interrupts send an INTERRUPT message (from HARDWARE) to a
   registered handler process.  The default beheviour is to disable
   the relevant IRQ in the interrupt handler, so that it can be
   re-enabled in the handler once it has reacted to the interrupt.

   We only deal with the 32 interrupts >= 0, not the 16 exceptions
   that are < 0 this way. */

static int os_handler[32];

/* connect -- connect the current process to an IRQ */
void connect(int irq) {
    if (irq < 0) panic("Can't connect to CPU exceptions");
    os_current->p_priority = 0;
    os_handler[irq] = os_current->p_pid;
    enable_irq(irq);
}

/* priority -- set process priority */
void priority(int p) {
    if (p < 0 || p > P_LOW) panic("Bad priority %d\n", p);
    os_current->p_priority = p;
}

/* interrupt -- send interrupt message */
void interrupt(int dest) {
    struct proc *pdest = os_ptable[dest];

    if (accept(pdest, INTERRUPT)) {
        // Receiver is waiting for an interrupt
        deliver(pdest->p_message, HARDWARE, INTERRUPT, NULL);

        make_ready(pdest, P_HANDLER);
        if (os_current->p_priority > 0) {
            // Preempt lower-priority process
            reschedule();
        }
    } else {
        // Let's hope it's not urgent!
        pdest->p_pending = 1;
    }
}

/* All interrupts are handled by this common handler, which disables the
   interrupt temporarily, then sends or queues a message to the registered
   handler task.  Normally the handler task will deal with the cause of the
   interrupt, then re-enable it. */

/* default_handler -- handle for most interrupts */
void default_handler(void) {
    int irq = active_irq(), task;
    if (irq < 0 || (task = os_handler[irq]) == 0)
        panic("Unexpected interrupt %d", irq);
    disable_irq(irq);
    interrupt(task);
}

/* hardfault_handler -- substitutes for the definition in startup.c */
void hardfault_handler(void) {
    panic("HardFault");
}


/* INITIALISATION */

#define IDLE_STACK 128

static struct proc *init_proc(char *name, unsigned stksize) {
    int pid;
    struct proc *p;
    unsigned char *stack;
    unsigned *sp;

    if (os_nprocs >= NPROCS)
        panic("Too many processes");

    pid = os_nprocs++;
    p = os_ptable[pid] = new_proc();
    stack = sbrk(stksize);
    sp = (unsigned *) &stack[stksize];

    /* Blank out the stack space to help detect overflow */
    for (unsigned *p = (unsigned *) stack; p < sp; p++) *p = BLANK;

    p->p_pid = pid;
    strncpy(p->p_name, name, 15);
    p->p_name[15] = '\0';
    p->p_sp = sp;
    p->p_stack = stack;
    p->p_stksize = stksize;
    p->p_state = ACTIVE;
    p->p_priority = P_LOW;
    p->p_waiting = 0;
    p->p_pending = 0;
    p->p_msgtype = ANY;
    p->p_message = NULL;
    p->p_next = NULL;

    return p;
}

/* os_init -- set up initial values */
void os_init(void) {
    // Create idle task as process 0
    idle_proc = init_proc("IDLE", IDLE_STACK);
    idle_proc->p_state = IDLING;
    idle_proc->p_priority = 3;
}

#define INIT_PSR 0x01000000     /* Thumb bit is set */

// These match the frame layout in mpx.s, and the hardware
#define R0_SAVE 8
#define R1_SAVE 9
#define R2_SAVE 10
#define LR_SAVE 13
#define PC_SAVE 14
#define PSR_SAVE 15

#define roundup(x, n) (((x) + ((n)-1)) & ~((n)-1))

/* start -- initialise process to run later */
int start(char *name, void (*body)(int), int arg, int stksize) {
    struct proc *p = init_proc(name, roundup(stksize, 8));

    if (os_current != NULL)
        panic("start() called after scheduler startup");

    /* Fake an exception frame */
    unsigned *sp = p->p_sp - 16;
    memset(sp, 0, 64);
    sp[PSR_SAVE] = INIT_PSR;
    sp[PC_SAVE] = (unsigned) body & ~0x1; // Activate the process body
    sp[LR_SAVE] = (unsigned) exit; // Make it return to exit()
    sp[R0_SAVE] = (unsigned) arg;  // Pass the supplied argument in R0
    p->p_sp = sp;

    make_ready(p, p->p_priority);
    return p->p_pid;
}

/* setstack -- enter thread mode with specified stack */
void setstack(unsigned *sp);

/* os_start -- start up the process scheduler */
void os_start(void) {
    // The main program morphs into the idle process.  The intial stack
    // becomes the kernel stack, and the idle process gets its own small
    // stack.

    os_current = idle_proc;
    setstack(os_current->p_sp);
    yield();                    // Pick a real process to run

    // Idle only runs again when there's nothing to do.
    while (1) {
        pause();                // Wait for an interrupt
    }
}


/* SYSTEM CALL INTERFACE */

// System call numbers
#define SYS_YIELD 0
#define SYS_SEND 1
#define SYS_RECEIVE 2
#define SYS_SENDREC 3
#define SYS_EXIT 4
#define SYS_DUMP 5

#define arg(i, t) ((t) psp[R0_SAVE+(i)])

/* system_call -- entry from system call traps */
unsigned *system_call(unsigned *psp) {
    short *pc = (short *) psp[PC_SAVE]; // Program counter
    int op = pc[-1] & 0xff;      // Syscall number from svc instruction

    // Save sp of the current process
    os_current->p_sp = psp;

    switch (op) {
    case SYS_YIELD:
        make_ready(os_current, os_current->p_priority);
        choose_proc();
        break;

    case SYS_SEND:
        mini_send(arg(0, int), arg(1, int), arg(2, message *));
        break;

    case SYS_RECEIVE:
        mini_receive(arg(0, int), arg(1, message *));
        break;

    case SYS_SENDREC:
        mini_sendrec(arg(0, int), arg(1, int), arg(2, message *));
        break;

    case SYS_EXIT:
        os_current->p_state = DEAD;
        choose_proc();
        break;

    case SYS_DUMP:
        /* Invoking microbian_dump as a system call means that its own
           stack space is taken from the system stack rather than the
           stack of the current process. */
        microbian_dump();
        break;

    default:
        panic("Unknown syscall %d", op);
    }

    // Return sp for next process to run
    return os_current->p_sp;
}

/* cxt_switch -- context switch following interrupt */
unsigned *cxt_switch(unsigned *psp) {
    os_current->p_sp = psp;
    make_ready(os_current, os_current->p_priority);
    choose_proc();
    return os_current->p_sp;
}


/* SYSTEM CALL STUBS */

/* Each function defined here leaves its arguments in r0, r1, etc., and
   executes an svc instruction with operand equal to the system call
   number.  After state has been saved, system_call() is invoked and
   retrieves the call number and arguments from the exception frame.
   Calls to these functions must not be inlined, or the arguments will
   not be found in the right places. */

#define NOINLINE __attribute((noinline))

void NOINLINE yield(void) {
     syscall(SYS_YIELD);
}

void NOINLINE send(int dest, int type, message *msg) {
     syscall(SYS_SEND);
}

void NOINLINE receive(int type, message *msg) {
     syscall(SYS_RECEIVE);
}

void NOINLINE sendrec(int dest, int type, message *msg) {
     syscall(SYS_SENDREC);
}

void NOINLINE exit(void) {
     syscall(SYS_EXIT);
}

void NOINLINE dump(void) {
     syscall(SYS_DUMP);
}


/* DEBUG PRINTING */

/* The routines here work by disabling interrupts and then polling: they
   should be used only for debugging. */

/* delay_usec -- delay loop */
static void delay_usec(int usec) {
    int t = usec<<1;
    while (t > 0) {
        // 500nsec per iteration
        nop(); nop(); nop();
        t--;
    }
}
        
/* kprintf_setup -- set up UART connection to host */
static void kprintf_setup(void) {
    // Delay so any UART activity can cease
    delay_usec(2000);

    // Reconfigure the UART just to be sure
    UART_ENABLE = UART_ENABLE_Disabled;

    GPIO_DIRSET = BIT(USB_TX);
    GPIO_DIRCLR = BIT(USB_RX);
    SET_FIELD(GPIO_PINCNF[USB_TX], GPIO_PINCNF_PULL, GPIO_PULL_Pullup);
    SET_FIELD(GPIO_PINCNF[USB_RX], GPIO_PINCNF_PULL, GPIO_PULL_Pullup);

    UART_BAUDRATE = UART_BAUDRATE_9600; // 9600 baud
    UART_CONFIG = 0;                    // format 8N1
    UART_PSELTXD = USB_TX;              // choose pins
    UART_PSELRXD = USB_RX;
    UART_ENABLE = UART_ENABLE_Enabled;
    UART_STARTTX = 1;
    UART_STARTRX = 1;
    UART_RXDRDY = 0;
}

/* kputc -- send output character */
static void kputc(char ch) {
    UART_TXD = ch;
    while (! UART_TXDRDY) { }
    UART_TXDRDY = 0;
}

/* kprintf_internal -- internal version of kprintf */
static void kprintf_internal(char *fmt, ...) {
    va_list va;
    va_start(va, fmt);
    do_print(kputc, fmt, va);
    va_end(va);
}

/* kprintf -- printf variant for debugging (disables interrupts) */
void kprintf(char *fmt, ...) {
    va_list va;

    lock();
    kprintf_setup();

    va_start(va, fmt);
    do_print(kputc, fmt, va);
    va_end(va);

    restore();
    // Caller gets a UART interrupt if enabled.
}

/* panic -- the unusual has happened.  Did you think it impossible? */
void panic(char *fmt, ...) {
    va_list va;
     
    lock();
    kprintf_setup();     

    kprintf_internal("\r\nPanic: ");
    va_start(va, fmt);
    do_print(kputc, fmt, va);
    va_end(va);
    if (os_current != NULL)
         kprintf_internal(" in process %s", os_current->p_name);
    kprintf_internal("\r\n");

    spin();
}

/* badmesg -- default case for switches on message type */
void badmesg(int type) {
     panic("Bad message type %d", type);
}