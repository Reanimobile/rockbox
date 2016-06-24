/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id: pmu-nano2g.c 27752 2010-08-08 10:49:32Z bertrik $
 *
 * Copyright © 2008 Rafaël Carré
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include "config.h"
#include "kernel.h"
#include "thread.h"

#include "pmu-target.h"
#include "i2c-s5l8702.h"
#include "gpio-s5l8702.h"


static struct mutex pmu_adc_mutex;

int pmu_read_multiple(int address, int count, unsigned char* buffer)
{
    return i2c_read(0, 0xe6, address, count, buffer);
}

int pmu_write_multiple(int address, int count, unsigned char* buffer)
{
    return i2c_write(0, 0xe6, address, count, buffer);
}

unsigned char pmu_read(int address)
{
    unsigned char tmp;

    pmu_read_multiple(address, 1, &tmp);

    return tmp;
}

int pmu_write(int address, unsigned char val)
{
    return pmu_write_multiple(address, 1, &val);
}

int pmu_read_adc(unsigned int adc)
{
    int data = 0;
    mutex_lock(&pmu_adc_mutex);
    pmu_write(0x54, 5 | (adc << 4));
    while ((data & 0x80) == 0)
    {
        yield();
        data = pmu_read(0x57);
    }
    int value = (pmu_read(0x55) << 2) | (data & 3);
    mutex_unlock(&pmu_adc_mutex);
    return value;
}

/* millivolts */
int pmu_read_battery_voltage(void)
{
    return (pmu_read_adc(1) * 2000 / 1023) + 2250;
}

/* milliamps */
int pmu_read_battery_current(void)
{
//TODO: Figure out how to read the battery current
//    return pmu_read_adc(2);
    return 0;
}

void pmu_ldo_on_in_standby(unsigned int ldo, int onoff)
{
    if (ldo < 4)
    {
        unsigned char newval = pmu_read(0x3B) & ~(1 << (2 * ldo));
        if (onoff) newval |= 1 << (2 * ldo);
        pmu_write(0x3B, newval);
    }
    else if (ldo < 8)
    {
        unsigned char newval = pmu_read(0x3C) & ~(1 << (2 * (ldo - 4)));
        if (onoff) newval |= 1 << (2 * (ldo - 4));
        pmu_write(0x3C, newval);
    }
}

void pmu_ldo_set_voltage(unsigned int ldo, unsigned char voltage)
{
    if (ldo > 6) return;
    pmu_write(0x2d + (ldo << 1), voltage);
}

void pmu_hdd_power(bool on)
{
    pmu_write(0x1b, on ? 1 : 0);
}

void pmu_ldo_power_on(unsigned int ldo)
{
    if (ldo > 6) return;
    pmu_write(0x2e + (ldo << 1), 1);
}

void pmu_ldo_power_off(unsigned int ldo)
{
    if (ldo > 6) return;
    pmu_write(0x2e + (ldo << 1), 0);
}

void pmu_set_wake_condition(unsigned char condition)
{
    pmu_write(0xd, condition);
}

void pmu_enter_standby(void)
{
    pmu_write(0xc, 1);
}

void pmu_read_rtc(unsigned char* buffer)
{
    pmu_read_multiple(0x59, 7, buffer);
}

void pmu_write_rtc(unsigned char* buffer)
{
    pmu_write_multiple(0x59, 7, buffer);
}

/*
 * eINT
 */
#define Q_EINT  0

static char pmu_thread_stack[DEFAULT_STACK_SIZE/4];
static struct event_queue pmu_queue;
static unsigned char ints_msk[6];

static void pmu_eint_isr(struct eint_handler*);

static struct eint_handler pmu_eint = {
    .gpio_n = GPIO_EINT_PMU,
    .type   = EIC_INTTYPE_LEVEL,
    .level  = EIC_INTLEVEL_LOW,
    .isr    = pmu_eint_isr,
};

static int pmu_input_holdswitch;

int pmu_holdswitch_locked(void)
{
   return pmu_input_holdswitch;
}

#ifdef IPOD_ACCESSORY_PROTOCOL
static int pmu_input_accessory;

int pmu_accessory_present(void)
{
   return pmu_input_accessory;
}
#endif

#if CONFIG_CHARGING
static int pmu_input_firewire;

int pmu_firewire_present(void)
{
   return pmu_input_firewire;
}

static void pmu_read_inputs_mbcs(void)
{
    pmu_input_firewire = !!(pmu_read(PCF5063X_REG_MBCS1)
                                & PCF5063X_MBCS1_ADAPTPRES);
}
#endif

static void pmu_read_inputs_gpio(void)
{
    pmu_input_holdswitch = !(pmu_read(PCF50635_REG_GPIOSTAT)
                                & PCF50635_GPIOSTAT_GPIO2);
}

static void pmu_read_inputs_ooc(void)
{
    unsigned char oocstat = pmu_read(PCF5063X_REG_OOCSTAT);
    if (oocstat & PCF5063X_OOCSTAT_EXTON2)
        usb_insert_int();
    else
        usb_remove_int();
#ifdef IPOD_ACCESSORY_PROTOCOL
    pmu_input_accessory = !(oocstat & PCF5063X_OOCSTAT_EXTON3);
#endif
}

static void pmu_eint_isr(struct eint_handler *h)
{
     eint_unregister(h);
     queue_post(&pmu_queue, Q_EINT, 0);
}

static void NORETURN_ATTR pmu_thread(void)
{
    struct queue_event ev;
    unsigned char ints[6];

    while (true)
    {
        queue_wait_w_tmo(&pmu_queue, &ev, TIMEOUT_BLOCK);
        switch (ev.id)
        {
            case Q_EINT:
                /* read (clear) PMU interrupts, this will also
                   raise the PMU IRQ pin */
                pmu_read_multiple(PCF5063X_REG_INT1, 2, ints);
                ints[5] = pmu_read(PCF50635_REG_INT6);

#if CONFIG_CHARGING
                if (ints[0] & ~ints_msk[0]) pmu_read_inputs_mbcs();
#endif
                if (ints[1] & ~ints_msk[1]) pmu_read_inputs_ooc();
                if (ints[5] & ~ints_msk[5]) pmu_read_inputs_gpio();

                eint_register(&pmu_eint);
                break;

            case SYS_TIMEOUT:
                break;
        }
    }
}

/* main init */
void pmu_init(void)
{
    mutex_init(&pmu_adc_mutex);
    queue_init(&pmu_queue, false);

    create_thread(pmu_thread,
            pmu_thread_stack, sizeof(pmu_thread_stack), 0,
            "PMU" IF_PRIO(, PRIORITY_SYSTEM) IF_COP(, CPU));

    /* configure PMU interrutps */
    for (int i = 0; i < 6; i++)
        ints_msk[i] = 0xff;

#if CONFIG_CHARGING
    ints_msk[0] &= ~PCF5063X_INT1_ADPINS &    /* FireWire */
                   ~PCF5063X_INT1_ADPREM;
#endif
    ints_msk[1] &= ~PCF5063X_INT2_EXTON2R &   /* USB */
                   ~PCF5063X_INT2_EXTON2F;
#ifdef IPOD_ACCESSORY_PROTOCOL
    ints_msk[1] &= ~PCF5063X_INT2_EXTON3R &   /* Accessory */
                   ~PCF5063X_INT2_EXTON3F;
#endif
    ints_msk[5] &= ~PCF50635_INT6_GPIO2;      /* Holdswitch */

    pmu_write_multiple(PCF5063X_REG_INT1M, 5, ints_msk);
    pmu_write(PCF50635_REG_INT6M, ints_msk[5]);

    /* clear all */
    unsigned char ints[5];
    pmu_read_multiple(PCF5063X_REG_INT1, 5, ints);
    pmu_read(PCF50635_REG_INT6);

    /* get initial values */
#if CONFIG_CHARGING
    pmu_read_inputs_mbcs();
#endif
    pmu_read_inputs_ooc();
    pmu_read_inputs_gpio();

    eint_register(&pmu_eint);
}

/*
 * preinit
 */
int pmu_rd_multiple(int address, int count, unsigned char* buffer)
{
    return i2c_rd(0, 0xe6, address, count, buffer);
}

int pmu_wr_multiple(int address, int count, unsigned char* buffer)
{
    return i2c_wr(0, 0xe6, address, count, buffer);
}

unsigned char pmu_rd(int address)
{
    unsigned char val;
    pmu_rd_multiple(address, 1, &val);
    return val;
}

int pmu_wr(int address, unsigned char val)
{
    return pmu_wr_multiple(address, 1, &val);
}

void pmu_preinit(void)
{
    static const char init_data[] =
    {
        /* reset OOC shutdown register */
        PCF5063X_REG_OOCSHDWN,  0x0,

        /* LDO_UNK1: 3000 mV, enabled */
        PCF5063X_REG_LDO1OUT,   0x15,
        PCF5063X_REG_LDO1ENA,   0x1,

        /* LDO_UNK2: 3000 mV, enabled */
        PCF5063X_REG_LDO2OUT,   0x15,
        PCF5063X_REG_LDO2ENA,   0x1,

        /* LDO_LCD: 3000 mV, enabled */
        PCF5063X_REG_LDO3OUT,   0x15,
        PCF5063X_REG_LDO3ENA,   0x1,

        /* LDO_CODEC: 1800 mV, enabled */
        PCF5063X_REG_LDO4OUT,   0x9,
        PCF5063X_REG_LDO4ENA,   0x1,

        /* LDO_UNK5: 3000 mV, disabled */
        PCF5063X_REG_LDO5OUT,   0x15,
        PCF5063X_REG_LDO5ENA,   0x0,

        /* LDO_CWHEEL: 3000 mV, ON when GPIO2 High */
        PCF5063X_REG_LDO6OUT,   0x15,
        PCF5063X_REG_LDO6ENA,   0x4,

        /* LDO_ACCY: 3300 mV, disabled */
        PCF5063X_REG_HCLDOOUT,  0x18,
        PCF5063X_REG_HCLDOENA,  0x0,

        /* LDO_CWHEEL is ON in STANDBY state,
           LDO_CWHEEL and MEMLDO are ON in UNKNOWN state (TBC) */
        PCF5063X_REG_STBYCTL1,  0x0,
        PCF5063X_REG_STBYCTL2,  0x8c,

        /* GPIO1,2 = input, GPIO3 = output */
        PCF5063X_REG_GPIOCTL,   0x3,
        PCF5063X_REG_GPIO1CFG,  0x0,
        PCF5063X_REG_GPIO2CFG,  0x0,

        /* DOWN2 converter (SDRAM): 1800 mV, enabled,
           startup current limit = 15mA*0x10 (TBC) */
        PCF5063X_REG_DOWN2OUT,  0x2f,
        PCF5063X_REG_DOWN2ENA,  0x1,
        PCF5063X_REG_DOWN2CTL,  0x0,
        PCF5063X_REG_DOWN2MXC,  0x10,

        /* MEMLDO: 1800 mV, enabled */
        PCF5063X_REG_MEMLDOOUT, 0x9,
        PCF5063X_REG_MEMLDOENA, 0x1,

        /* AUTOLDO (HDD): 3400 mV, disabled,
           limit = 1000 mA (40mA*0x19), limit always active */
        PCF5063X_REG_AUTOOUT,   0x6f,
#ifdef BOOTLOADER
        PCF5063X_REG_AUTOENA,   0x0,
#endif
        PCF5063X_REG_AUTOCTL,   0x0,
        PCF5063X_REG_AUTOMXC,   0x59,

        /* Vsysok = 3100 mV */
        PCF5063X_REG_SVMCTL,    0x8,

        /* Reserved */
        0x58,                   0x0,

        /* Mask all PMU interrupts */
        PCF5063X_REG_INT1M,     0xff,
        PCF5063X_REG_INT2M,     0xff,
        PCF5063X_REG_INT3M,     0xff,
        PCF5063X_REG_INT4M,     0xff,
        PCF5063X_REG_INT5M,     0xff,
        PCF50635_REG_INT6M,     0xff,

        /* Wakeup on rising edge for EXTON1 and EXTON2,
           wakeup on falling edge for EXTON3 and !ONKEY,
           wakeup on RTC alarm, wakeup on adapter insert,
           Vbat status has no effect in state machine */
        PCF5063X_REG_OOCWAKE,   0xdf,
        PCF5063X_REG_OOCTIM1,   0xaa,
        PCF5063X_REG_OOCTIM2,   0x4a,
        PCF5063X_REG_OOCMODE,   0x5,
        PCF5063X_REG_OOCCTL,    0x27,

        /* GPO selection = LED external NFET drive signal */
        PCF5063X_REG_GPOCFG,    0x1,
        /* LED converter OFF, overvoltage protection enabled,
           OCP limit is 500 mA, led_dimstep = 16*0x6/32768 */
#ifdef BOOTLOADER
        PCF5063X_REG_LEDENA,    0x0,
#endif
        PCF5063X_REG_LEDCTL,    0x5,
        PCF5063X_REG_LEDDIM,    0x6,

        /* end marker */
        0
    };

    const char* ptr;
    for (ptr = init_data; *ptr != 0; ptr += 2)
        pmu_wr(ptr[0], ptr[1]);

    /* clear PMU interrupts */
    unsigned char rd_buf[5];
    pmu_rd_multiple(PCF5063X_REG_INT1, 5, rd_buf);
    pmu_rd(PCF50635_REG_INT6);
}
