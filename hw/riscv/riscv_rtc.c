/*
 * QEMU RISC-V timer, instret counter support
 *
 * Author: Sagar Karandikar, sagark@eecs.berkeley.edu
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/riscv/cpudevs.h"
#include "hw/riscv/riscv_clint.h"
#include "hw/riscv/riscv_rtc_internal.h"
#include "hw/riscv/riscv_rtc.h"
#include "hw/riscv/riscv_hart.h"
#include "hw/riscv/soc.h"
#include "qemu/timer.h"

/*#define TIMER_DEBUGGING_RISCV */

/* this is the "right value" for defaults in pk/linux
   see pk/sbi_entry.S and arch/riscv/kernel/time.c call to
   clockevents_config_and_register */
/* TODO: EMDALO RESOLVE */
/* #define TIMER_FREQ (10 * 1000 * 1000) */
#define TIMER_FREQ (1 * 100 * 1000)
/* CPU_FREQ is for instret approximation - say we're running at 1 BIPS */
/* #define CPU_FREQ (1000 * 1000 * 1000) */
#define CPU_FREQ (10 * 1000 * 1000)

inline uint64_t rtc_read(CPURISCVState *env)
{
    return muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), TIMER_FREQ,
                    NANOSECONDS_PER_SECOND);
}

inline uint64_t instret_read(CPURISCVState *env)
{
    return muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), CPU_FREQ,
                    NANOSECONDS_PER_SECOND);
}

/*
 * Called when timecmp is written to update the QEMU timer or immediately
 * trigger timer interrupt if mtimecmp <= current timer value.
 */
static inline void cpu_riscv_timer_update(CPURISCVState *env)
{
    uint64_t next;
    uint64_t diff;

    uint64_t rtc_r = rtc_read(env);

#ifdef TIMER_DEBUGGING_RISCV
    printf("timer update: mtimecmp %016lx, timew %016lx\n",
            env->timecmp, rtc_r);
#endif

    if (env->timecmp <= rtc_r) {
        /* if we're setting an MTIMECMP value in the "past",
           immediately raise the timer interrupt */
        env->mip |= MIP_MTIP;
        if (env->mie & MIP_MTIP) {
            qemu_irq_raise(MTIP_IRQ);
        }
        return;
    }

    /* otherwise, set up the future timer interrupt */
    diff = env->timecmp - rtc_r;
    /* back to ns (note args switched in muldiv64) */
    next = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
        muldiv64(diff, NANOSECONDS_PER_SECOND, TIMER_FREQ);

    /* TODO: EMDALO: HACK: REMOVE */
    if (env->timecmp >= 0xffffffffffffff) {
        next = 0xfffffffffffff;
    }

    timer_mod(env->timer, next);
}

/*
 * Called by the callback used when the timer set using timer_mod expires.
 * Should raise the timer interrupt line
 */
static inline void cpu_riscv_timer_expire(CPURISCVState *env)
{
    /* TODO: EMDALO HACK: RESOLVE */
    static int foo;
    /* do not call update here */
    env->mip |= MIP_MTIP;
    if (foo == 20) {
        qemu_irq_raise(MTIP_IRQ);
        foo = 0;
    }
    foo++;
}

/* used in op_helper.c */
inline uint64_t cpu_riscv_read_instret(CPURISCVState *env)
{
    uint64_t retval = instret_read(env);
    return retval;
}

inline uint64_t cpu_riscv_read_rtc(CPURISCVState *env)
{
    uint64_t retval = rtc_read(env);
    return retval;
}

inline void write_timecmp(CPURISCVState *env, uint64_t value)
{
#ifdef TIMER_DEBUGGING_RISCV
    uint64_t rtc_r = rtc_read(env);
    printf("wrote mtimecmp %016lx, timew %016lx\n", value, rtc_r);
#endif

    env->timecmp = value;
    env->mip &= ~MIP_MTIP;
    cpu_riscv_timer_update(env);
}

/*
 * Callback used when the timer set using timer_mod expires.
 */
static void riscv_timer_cb(void *opaque)
{
    CPURISCVState *env;
    env = opaque;
    cpu_riscv_timer_expire(env);
}

/*
 * Initialize clock mechanism.
 */
void cpu_riscv_clock_init(CPURISCVState *env)
{
    env->timecmp = 0xfffffffffffffffe;
    env->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, &riscv_timer_cb, env);
}


#include "target-riscv/cpu.h"

static void timer_pre_save(void *opaque)
{
    return;
}

static int timer_post_load(void *opaque, int version_id)
{
    return 0;
}

const VMStateDescription vmstate_timer_rv = {
    .name = "rvtimer",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = timer_pre_save,
    .post_load = timer_post_load,
    .fields      = (VMStateField []) { /* TODO what */
        VMSTATE_END_OF_LIST()
    },
};

/* CPU wants to read rtc or timecmp register */
static uint64_t timer_mm_read(void *opaque, hwaddr addr, unsigned size)
{
    TIMERState *timerstate = opaque;
    uint8_t index = addr >> 3;

    if (addr == CLINT_REL_MTIME_OFFSET) {
        /* rtc */
        timerstate->temp_rtc_val = rtc_read(timerstate->env[index]);
        return timerstate->temp_rtc_val & 0xFFFFFFFF;
    } else if (addr == CLINT_REL_MTIME_OFFSET + 4) {
        /* rtc */
        return (timerstate->temp_rtc_val >> 32) & 0xFFFFFFFF;
    } else if (addr % 8 == 0) {
        /* timecmp */
        return timerstate->timecmp_lower[index];
    } else if (addr % 4 == 0) {
        /* timecmp */
        return timerstate->timecmp_upper[index];
    } else {
        printf("Invalid timer register address %016" PRIx64 "\n",
               (uint64_t)addr);
        exit(1);
    }
    return 0;
}

/* CPU wrote to rtc or timecmp register */
static void timer_mm_write(void *opaque, hwaddr addr, uint64_t value,
        unsigned size)
{
    TIMERState *timerstate = opaque;

    uint8_t index = addr >> 3;

    if (addr == CLINT_REL_MTIME_OFFSET) {
        /*rtc */
        printf("RTC WRITE NOT IMPL\n");
        exit(1);
    } else if (addr == CLINT_REL_MTIME_OFFSET + 4) {
        /*rtc */
        printf("RTC WRITE NOT IMPL\n");
        exit(1);
    } else if (addr % 8 == 0) {
        /* timecmp */
        timerstate->timecmp_lower[index] = value & 0xFFFFFFFF;
    } else if (addr % 4 == 0) {
        /* timecmp */
        timerstate->timecmp_upper[index] = value & 0xffffffff;
        write_timecmp(timerstate->env[index], value << 32 |
                timerstate->timecmp_lower[index]);
    } else {
        printf("Invalid timer register address %016" PRIx64 "\n",
               (uint64_t)addr);
        exit(1);
    }
}

static const MemoryRegionOps timer_mm_ops[3] = {
    [DEVICE_LITTLE_ENDIAN] = {
        .read = timer_mm_read,
        .write = timer_mm_write,
        .endianness = DEVICE_LITTLE_ENDIAN,
    },
};

TIMERState *timer_mm_init(MemoryRegion *address_space, hwaddr base,
                          CPURISCVState *env)
{
    TIMERState *timerstate;
    int i;

    timerstate = g_malloc0(sizeof(TIMERState));
    timerstate->env = g_malloc0(sizeof(CPURISCVState *) * env->num_harts);
    timerstate->timecmp_lower = g_malloc0(sizeof(uint32_t) * env->num_harts);
    timerstate->timecmp_upper = g_malloc0(sizeof(uint32_t) * env->num_harts);

    for (i = 0; i < env->num_harts; i++) {
        timerstate->env[i] = hart_get_env(i);
        timerstate->timecmp_lower[i] = 0;
        timerstate->timecmp_upper[i] = 0;
    }
    timerstate->temp_rtc_val = 0;
    vmstate_register(NULL, base, &vmstate_timer_rv, timerstate);
    memory_region_init_io(&timerstate->io, NULL,
            &timer_mm_ops[DEVICE_LITTLE_ENDIAN],
            timerstate, "clint timer", CLINT_TIME_REG_SZ);
    memory_region_add_subregion(address_space, base, &timerstate->io);
    return timerstate;
}
