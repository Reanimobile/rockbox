/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright © 2013 Andrew Ryabinin
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

#include <stdbool.h>
#include "config.h"
#include "inttypes.h"
#include "power.h"
#include "panic.h"
#include "system.h"
#include "usb_core.h"   /* for usb_charging_maxcurrent_change */

void power_off(void)
{
    GPIO_PCDR |= (1<<0);
    GPIO_PCCON &= ~(1<<0);
    while(1);
}

void power_init(void)
{
    GPIO_PCDR |= (1<<0);
    GPIO_PCCON |= (1<<0);
}

unsigned int power_input_status(void)
{
    unsigned int status = POWER_INPUT_NONE;

    if (charging_state())
        status |= POWER_INPUT_MAIN_CHARGER;
    return status;
}

extern unsigned short pca9555_in_ports;

bool charging_state(void)
{
    return (((GPIO_PFDR&(1<<2)) == 0) && ((pca9555_in_ports&(1<<11)) == 0));
}

