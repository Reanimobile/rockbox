/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id: i2c-s5l8700.c 28589 2010-11-14 15:19:30Z theseven $
 *
 * Copyright (C) 2009 by Bertrik Sikken
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
#include "system.h"
#include "kernel.h"
#include "i2c-s5l8702.h"
#include "clocking-s5l8702.h"

/*  Driver for the s5l8700 built-in I2C controller in master mode

    Both the i2c_read and i2c_write function take the following arguments:
    * slave, the address of the i2c slave device to read from / write to
    * address, optional sub-address in the i2c slave (unused if -1)
    * len, number of bytes to be transfered
    * data, pointer to data to be transfered
    A return value < 0 indicates an error.

    Note:
    * blocks the calling thread for the entire duraton of the i2c transfer but
      uses wakeup_wait/wakeup_signal to allow other threads to run.
    * ACK from slave is not checked, so functions never return an error

    Fixme:
    * actually there is no STOP + i2c_off() on error
    * very rare random errors when reading and/or(?) writing registers on some
      builds/devices, hard to trace, not a 'delay' issue, it seems related
      with alignment of STRs and/or(?) LDRs, code cache lines, pipelines...
      The new code tries to mix STRs and LDRs at some points but ATM it is
      unknown if it might solve or mitigate the problem. Probably it could be
      really fixed using wait_rdy() before accessing any register, as OF does.
*/

/*  s5l8702 I2C controller is similar to s5l8700, known differences are:

    * IICCON[5] is not used in s5l8702.
    * IICCON[13:8] are used to enable interrupts.
      IICUNK20[13:8] are used to read the status and write-clear interrupts.
      Known interrupts:
       [13] STOP on bus (TBC)
       [12] START on bus (TBC)
       [8] byte transmited or received in Master mode (not tested in Slave)
    * IICCON[4] does not clear interrupts, it is enabled when a byte is
      transmited or received, in Master mode the tx/rx of the next byte
      starts when it is written as "1".
*/

static struct mutex i2c_mtx[2];

static void i2c_on(int bus)
{
    /* enable I2C clock */
    clockgate_enable(I2CCLKGATE(bus), true);

    IICCON(bus) = (0 << 8) | /* INT_EN = disabled */
                  (1 << 7) | /* ACK_GEN */
                  (0 << 6) | /* CLKSEL = PCLK/16 */
                  (7 << 0);  /* CK_REG */

    /* serial output on */
    IICSTAT(bus) = (1 << 4);
}

static void i2c_off(int bus)
{
    /* serial output off */
    IICSTAT(bus) = 0;

    /* disable I2C clock */
    clockgate_enable(I2CCLKGATE(bus), false);
}

void i2c_init()
{
    mutex_init(&i2c_mtx[0]);
    mutex_init(&i2c_mtx[1]);
}

int i2c_wr(int bus, unsigned char slave, int address, int len, const unsigned char *data)
{
    i2c_on(bus);
    long timeout = USEC_TIMER + 20000;

    /* START */
    IICDS(bus) = slave & ~1;
    IICSTAT(bus) = 0xF0;
    while ((IICCON(bus) & 0x10) == 0)
        if (TIME_AFTER(USEC_TIMER, timeout))
            return 1;

    if (address >= 0) {
        /* write address */
        IICDS(bus) = address;
        IICCON(bus) = IICCON(bus);
        while ((IICCON(bus) & 0x10) == 0)
            if (TIME_AFTER(USEC_TIMER, timeout))
                return 2;
    }

    /* write data */
    while (len--) {
        IICDS(bus) = *data++;
        IICCON(bus) = IICCON(bus);
        while ((IICCON(bus) & 0x10) == 0)
            if (TIME_AFTER(USEC_TIMER, timeout))
                return 4;
    }

    /* STOP */
    IICSTAT(bus) = 0xD0;
    IICCON(bus) = IICCON(bus);
    while ((IICSTAT(bus) & (1 << 5)) != 0)
        if (TIME_AFTER(USEC_TIMER, timeout))
            return 5;

    i2c_off(bus);
    return 0;
}

int i2c_rd(int bus, unsigned char slave, int address, int len, unsigned char *data)
{
    i2c_on(bus);
    long timeout = USEC_TIMER + 20000;

    if (address >= 0) {
        /* START */
        IICDS(bus) = slave & ~1;
        IICSTAT(bus) = 0xF0;
        while ((IICCON(bus) & 0x10) == 0)
            if (TIME_AFTER(USEC_TIMER, timeout))
                return 1;

        /* write address */
        IICDS(bus) = address;
        IICCON(bus) = IICCON(bus);
        while ((IICCON(bus) & 0x10) == 0)
            if (TIME_AFTER(USEC_TIMER, timeout))
                return 2;
    }

    /* (repeated) START */
    IICDS(bus) = slave | 1;
    IICSTAT(bus) = 0xB0;
    IICCON(bus) = IICCON(bus);
    while ((IICCON(bus) & 0x10) == 0)
        if (TIME_AFTER(USEC_TIMER, timeout))
            return 3;

    while (len--) {
        IICCON(bus) &= ~(len ? 0 : 0x80); /* ACK or NAK */
        while ((IICCON(bus) & 0x10) == 0)
            if (TIME_AFTER(USEC_TIMER, timeout))
                return 4;
        *data++ = IICDS(bus);
    }

    /* STOP */
    IICSTAT(bus) = 0x90;
    IICCON(bus) = IICCON(bus);
    while ((IICSTAT(bus) & (1 << 5)) != 0)
        if (TIME_AFTER(USEC_TIMER, timeout))
            return 5;

    i2c_off(bus);
    return 0;
}

unsigned long i2c_rd_err, i2c_wr_err;

int i2c_write(int bus, unsigned char slave, int address, int len, const unsigned char *data)
{
    int ret;
    mutex_lock(&i2c_mtx[bus]);
    ret = i2c_wr(bus, slave, address, len, data);
    mutex_unlock(&i2c_mtx[bus]);
    if (ret) i2c_wr_err++;
    return ret;
}

int i2c_read(int bus, unsigned char slave, int address, int len, unsigned char *data)
{
    int ret;
    mutex_lock(&i2c_mtx[bus]);
    ret = i2c_rd(bus, slave, address, len, data);
    mutex_unlock(&i2c_mtx[bus]);
    if (ret) i2c_rd_err++;
    return ret;
}

static void wait_rdy(int bus)
{
    while (IICUNK10(bus));
}

void i2c_preinit(int bus)
{
    clockgate_enable(I2CCLKGATE(bus), true);
    wait_rdy(bus);
    IICADD(bus) = 0x40;   /* own slave address */
    wait_rdy(bus);
    IICUNK14(bus) = 0;
    wait_rdy(bus);
    IICUNK18(bus) = 0;
    wait_rdy(bus);
    IICSTAT(bus) = 0x80;  /* master Rx mode, Tx/Rx off */
    wait_rdy(bus);
    IICCON(bus) = 0;
    wait_rdy(bus);
    IICSTAT(bus) = 0;     /* slave Rx mode, Tx/Rx off */
    wait_rdy(bus);
    clockgate_enable(I2CCLKGATE(bus), false);
}
