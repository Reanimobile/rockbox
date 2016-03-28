/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2016 by Vortex
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
#include "gcc_extensions.h"
#include "cpu.h"
#include "ata.h"
#include "ata_idle_notify.h"
#include "ata-sd-target.h"
#include "dma-target.h"
#include "disk.h"
#include "led.h"
#include "sdmmc.h"
#include "logf.h"
#include "sd.h"
#include "system.h"
#include "kernel.h"
#include "storage.h"
#include "string.h"
#include "usb.h"

static long               last_disk_activity = -1;
#if defined(CONFIG_STORAGE_MULTI) || defined(HAVE_HOTSWAP)
static int                sd_drive_nr = 0;
#endif
static tCardInfo          card[NUM_DRIVES];

static long               sd_stack[(DEFAULT_STACK_SIZE*2 + 0x1c0)/sizeof(long)];
static const char         sd_thread_name[] = "ata/sd";
static struct event_queue sd_queue;
static struct mutex       sd_mtx;
static struct semaphore   sd_wakeup;
static void               sd_thread(void) NORETURN_ATTR;

static int                use_4bit[NUM_DRIVES];
static int                num_6[NUM_DRIVES];
static int                sd2_0[NUM_DRIVES];

#define SD_DMA_ENABLE 1

#define UNALIGNED_NUM_SECTORS 256
static unsigned char aligned_buffer[UNALIGNED_NUM_SECTORS*SD_BLOCK_SIZE] __attribute__((aligned(4)));   /* align on word */

//#define DEBUG(x...)         logf(x)
#define DEBUG(x, ...)

/* volumes */
#define SD_SLOT_1  0       /* SD card 1 */
#define SD_SLOT_2  1       /* SD card 2 */

#define MSC_CHN(n) (2-n)

#define SD_IRQ_MASK(n)                \
do {                                  \
          REG_MSC_IMASK(n) = 0xffff;  \
          REG_MSC_IREG(n) = 0xffff;   \
} while (0)

/* Error codes */
enum sd_result_t
{
    SD_NO_RESPONSE        = -1,
    SD_NO_ERROR           = 0,
    SD_ERROR_OUT_OF_RANGE,
    SD_ERROR_ADDRESS,
    SD_ERROR_BLOCK_LEN,
    SD_ERROR_ERASE_SEQ,
    SD_ERROR_ERASE_PARAM,
    SD_ERROR_WP_VIOLATION,
    SD_ERROR_CARD_IS_LOCKED,
    SD_ERROR_LOCK_UNLOCK_FAILED,
    SD_ERROR_COM_CRC,
    SD_ERROR_ILLEGAL_COMMAND,
    SD_ERROR_CARD_ECC_FAILED,
    SD_ERROR_CC,
    SD_ERROR_GENERAL,
    SD_ERROR_UNDERRUN,
    SD_ERROR_OVERRUN,
    SD_ERROR_CID_CSD_OVERWRITE,
    SD_ERROR_STATE_MISMATCH,
    SD_ERROR_HEADER_MISMATCH,
    SD_ERROR_TIMEOUT,
    SD_ERROR_CRC,
    SD_ERROR_DRIVER_FAILURE,
};

/* Standard MMC/SD clock speeds */
#define MMC_CLOCK_SLOW    400000      /* 400 kHz for initial setup */
#define SD_CLOCK_FAST   24000000      /* 24 MHz for SD Cards */
#define SD_CLOCK_HIGH   48000000      /* 48 MHz for SD Cards */

/* Extra commands for state control */
/* Use negative numbers to disambiguate */
#define SD_CIM_RESET            -1

/* Proprietary commands, illegal/reserved according to SD Specification 2.00 */
    /* class 1 */
#define SD_READ_DAT_UNTIL_STOP  11   /* adtc [31:0]  dadr       R1  */

    /* class 3 */
#define SD_WRITE_DAT_UNTIL_STOP 20   /* adtc [31:0]  data addr  R1  */

    /* class 4 */
#define SD_PROGRAM_CID          26   /* adtc                    R1  */
#define SD_PROGRAM_CSD          27   /* adtc                    R1  */

    /* class 9 */
#define SD_GO_IRQ_STATE         40   /* bcr                     R5  */

/* Don't change the order of these; they are used in dispatch tables */
enum sd_rsp_t
{
    RESPONSE_NONE    = 0,
    RESPONSE_R1      = 1,
    RESPONSE_R1B     = 2,
    RESPONSE_R2_CID  = 3,
    RESPONSE_R2_CSD  = 4,
    RESPONSE_R3      = 5,
    RESPONSE_R4      = 6,
    RESPONSE_R5      = 7,
    RESPONSE_R6      = 8,
    RESPONSE_R7      = 9,
};

/*
  MMC status in R1
  Type
    e : error bit
    s : status bit
    r : detected and set for the actual command response
    x : detected and set during command execution. the host must poll
        the card by sending status command in order to read these bits.
  Clear condition
    a : according to the card state
    b : always related to the previous command. Reception of
        a valid command will clear it (with a delay of one command)
    c : clear by read
 */

#define R1_OUT_OF_RANGE        (1 << 31)    /* er, c */
#define R1_ADDRESS_ERROR       (1 << 30)    /* erx, c */
#define R1_BLOCK_LEN_ERROR     (1 << 29)    /* er, c */
#define R1_ERASE_SEQ_ERROR     (1 << 28)    /* er, c */
#define R1_ERASE_PARAM         (1 << 27)    /* ex, c */
#define R1_WP_VIOLATION        (1 << 26)    /* erx, c */
#define R1_CARD_IS_LOCKED      (1 << 25)    /* sx, a */
#define R1_LOCK_UNLOCK_FAILED  (1 << 24)    /* erx, c */
#define R1_COM_CRC_ERROR       (1 << 23)    /* er, b */
#define R1_ILLEGAL_COMMAND     (1 << 22)    /* er, b */
#define R1_CARD_ECC_FAILED     (1 << 21)    /* ex, c */
#define R1_CC_ERROR            (1 << 20)    /* erx, c */
#define R1_ERROR               (1 << 19)    /* erx, c */
#define R1_UNDERRUN            (1 << 18)    /* ex, c */
#define R1_OVERRUN             (1 << 17)    /* ex, c */
#define R1_CID_CSD_OVERWRITE   (1 << 16)    /* erx, c, CID/CSD overwrite */
#define R1_WP_ERASE_SKIP       (1 << 15)    /* sx, c */
#define R1_CARD_ECC_DISABLED   (1 << 14)    /* sx, a */
#define R1_ERASE_RESET         (1 << 13)    /* sr, c */
#define R1_STATUS(x)           (x & 0xFFFFE000)
#define R1_CURRENT_STATE(x)    ((x & 0x00001E00) >> 9)    /* sx, b (4 bits) */
#define R1_READY_FOR_DATA      (1 << 8)     /* sx, a */
#define R1_APP_CMD             (1 << 7)     /* sr, c */

/* These are unpacked versions of the actual responses */
struct sd_response_r1
{
    unsigned char  cmd;
    unsigned int   status;
};

struct sd_response_r3
{  
    unsigned int ocr;
};

#define SD_CARD_BUSY    0x80000000    /* Card Power up status bit */

struct sd_request
{
    int               index;      /* Slot index - used for CS lines */
    int               cmd;        /* Command to send */
    unsigned int      arg;        /* Argument to send */
    enum sd_rsp_t    rtype;      /* Response type expected */

    /* Data transfer (these may be modified at the low level) */
    unsigned short    nob;        /* Number of blocks to transfer*/
    unsigned short    block_len;  /* Block length */
    unsigned char     *buffer;    /* Data buffer */
    unsigned int      cnt;        /* Data length, for PIO */

    /* Results */
    unsigned char     response[18]; /* Buffer to store response - CRC is optional */
    enum sd_result_t result;
};

#define SD_OCR_ARG             0x00ff8000  /* Argument of OCR */

/***********************************************************************
 *  SD Events
 */
#define SD_EVENT_NONE            0x00    /* No events */
#define SD_EVENT_RX_DATA_DONE    0x01    /* Rx data done */
#define SD_EVENT_TX_DATA_DONE    0x02    /* Tx data done */
#define SD_EVENT_PROG_DONE       0x04    /* Programming is done */

/**************************************************************************
 * Utility functions
 **************************************************************************/

#define PARSE_U32(_buf,_index) \
    (((unsigned int)_buf[_index]) << 24) | (((unsigned int)_buf[_index+1]) << 16) | \
        (((unsigned int)_buf[_index+2]) << 8) | ((unsigned int)_buf[_index+3]);

#define PARSE_U16(_buf,_index) \
    (((unsigned short)_buf[_index]) << 8) | ((unsigned short)_buf[_index+1]);

static int sd_unpack_r1(struct sd_request *request, struct sd_response_r1 *r1)
{
    unsigned char *buf = request->response;

    if (request->result)
        return request->result;

    r1->cmd    = buf[0];
    r1->status = PARSE_U32(buf,1);

    DEBUG("sd_unpack_r1: cmd=%d status=%08x", r1->cmd, r1->status);

    if (R1_STATUS(r1->status)) {
        if (r1->status & R1_OUT_OF_RANGE)       return SD_ERROR_OUT_OF_RANGE;
        if (r1->status & R1_ADDRESS_ERROR)      return SD_ERROR_ADDRESS;
        if (r1->status & R1_BLOCK_LEN_ERROR)    return SD_ERROR_BLOCK_LEN;
        if (r1->status & R1_ERASE_SEQ_ERROR)    return SD_ERROR_ERASE_SEQ;
        if (r1->status & R1_ERASE_PARAM)        return SD_ERROR_ERASE_PARAM;
        if (r1->status & R1_WP_VIOLATION)       return SD_ERROR_WP_VIOLATION;
        //if (r1->status & R1_CARD_IS_LOCKED)     return SD_ERROR_CARD_IS_LOCKED;
        if (r1->status & R1_LOCK_UNLOCK_FAILED) return SD_ERROR_LOCK_UNLOCK_FAILED;
        if (r1->status & R1_COM_CRC_ERROR)      return SD_ERROR_COM_CRC;
        if (r1->status & R1_ILLEGAL_COMMAND)    return SD_ERROR_ILLEGAL_COMMAND;
        if (r1->status & R1_CARD_ECC_FAILED)    return SD_ERROR_CARD_ECC_FAILED;
        if (r1->status & R1_CC_ERROR)           return SD_ERROR_CC;
        if (r1->status & R1_ERROR)              return SD_ERROR_GENERAL;
        if (r1->status & R1_UNDERRUN)           return SD_ERROR_UNDERRUN;
        if (r1->status & R1_OVERRUN)            return SD_ERROR_OVERRUN;
        if (r1->status & R1_CID_CSD_OVERWRITE)  return SD_ERROR_CID_CSD_OVERWRITE;
    }

    if (buf[0] != request->cmd)
        return SD_ERROR_HEADER_MISMATCH;

    /* This should be last - it's the least dangerous error */

    return 0;
}

static int sd_unpack_r6(struct sd_request *request, struct sd_response_r1 *r1, unsigned long *rca)
{
    unsigned char *buf = request->response;

    if (request->result)
        return request->result;

    *rca = PARSE_U16(buf,1);  /* Save RCA returned by the SD Card */

    *(buf+1) = 0;
    *(buf+2) = 0;

    return sd_unpack_r1(request, r1);
}

static int sd_unpack_r3(struct sd_request *request, struct sd_response_r3 *r3)
{
    unsigned char *buf = request->response;

    if (request->result) return request->result;

    r3->ocr = PARSE_U32(buf,1);
    DEBUG("sd_unpack_r3: ocr=%08x", r3->ocr);

    if (buf[0] != 0x3f)
        return SD_ERROR_HEADER_MISMATCH;

    return 0;
}

/* Stop the MMC clock and wait while it happens */
static inline int jz_sd_stop_clock(const int drive)
{
    register int timeout = 1000;

    //DEBUG("stop MMC clock");
    REG_MSC_STRPCL(MSC_CHN(drive)) = MSC_STRPCL_CLOCK_CONTROL_STOP;

    while (timeout && (REG_MSC_STAT(MSC_CHN(drive)) & MSC_STAT_CLK_EN))
    {
        timeout--;
        if (timeout == 0)
        {
            DEBUG("Timeout on stop clock waiting");
            return SD_ERROR_TIMEOUT;
        }
        udelay(1);
    }
    //DEBUG("clock off time is %d microsec", timeout);
    return SD_NO_ERROR;
}

/* Start the MMC clock and operation */
static inline int jz_sd_start_clock(const int drive)
{
    REG_MSC_STRPCL(MSC_CHN(drive)) = MSC_STRPCL_CLOCK_CONTROL_START | MSC_STRPCL_START_OP;
    return SD_NO_ERROR;
}

static int jz_sd_check_status(const int drive, struct sd_request *request)
{
    (void)request;
    unsigned int status = REG_MSC_STAT(MSC_CHN(drive));

    /* Checking for response or data timeout */
    if (status & (MSC_STAT_TIME_OUT_RES | MSC_STAT_TIME_OUT_READ))
    {
        DEBUG("SD timeout, MSC_STAT 0x%x CMD %d", status,
               request->cmd);
        return SD_ERROR_TIMEOUT;
    }

    /* Checking for CRC error */
    if (status &
        (MSC_STAT_CRC_READ_ERROR | MSC_STAT_CRC_WRITE_ERROR |
         MSC_STAT_CRC_RES_ERR))
    {
        DEBUG("SD CRC error, MSC_STAT 0x%x", status);
        return SD_ERROR_CRC;
    
    }
    
    
    /* Checking for FIFO empty */
    /*if(status & MSC_STAT_DATA_FIFO_EMPTY && request->rtype != RESPONSE_NONE)
    {
        DEBUG("SD FIFO empty, MSC_STAT 0x%x", status);
        return SD_ERROR_UNDERRUN;
    }*/

    return SD_NO_ERROR;
}

/* Obtain response to the command and store it to response buffer */
static void jz_sd_get_response(const int drive, struct sd_request *request)
{
    int i;
    unsigned char *buf;
    unsigned int data;

    DEBUG("fetch response for request %d, cmd %d", request->rtype,
          request->cmd);
    buf = request->response;
    request->result = SD_NO_ERROR;

    switch (request->rtype)
    {
        case RESPONSE_R1:
        case RESPONSE_R1B:
        case RESPONSE_R7:
        case RESPONSE_R6:
        case RESPONSE_R3:
        case RESPONSE_R4:
        case RESPONSE_R5:
        {
            data = REG_MSC_RES(MSC_CHN(drive));
            buf[0] = (data >> 8) & 0xff;
            buf[1] = data & 0xff;
            data = REG_MSC_RES(MSC_CHN(drive));
            buf[2] = (data >> 8) & 0xff;
            buf[3] = data & 0xff;
            data = REG_MSC_RES(MSC_CHN(drive));
            buf[4] = data & 0xff;

            DEBUG("request %d, response [%02x %02x %02x %02x %02x]",
                  request->rtype, buf[0], buf[1], buf[2],
                  buf[3], buf[4]);
            break;
        }
        case RESPONSE_R2_CID:
        case RESPONSE_R2_CSD:
        {
            for (i = 0; i < 16; i += 2)
            {
                data = REG_MSC_RES(MSC_CHN(drive));
                buf[i] = (data >> 8) & 0xff;
                buf[i + 1] = data & 0xff;
            }
            DEBUG("request %d, response []", request->rtype);
            break;
        }
        case RESPONSE_NONE:
            DEBUG("No response");
            break;

        default:
            DEBUG("unhandled response type for request %d",
                  request->rtype);
            break;
    }
}

#if SD_DMA_ENABLE
static void jz_sd_receive_data_dma(const int drive, struct sd_request *req)
{
    unsigned int waligned = (((unsigned int)req->buffer & 0x3) == 0);    /* word aligned ? */
    unsigned int size = req->block_len * req->nob;

    if (!waligned)
    {
        if (size > UNALIGNED_NUM_SECTORS*SD_BLOCK_SIZE)
            size = UNALIGNED_NUM_SECTORS*SD_BLOCK_SIZE;
    }

    /* flush dcache */
    dma_cache_wback_inv((unsigned long) req->buffer, size);
    /* setup dma channel */
    REG_DMAC_DCCSR(DMA_SD_RX_CHANNEL) = 0;
    REG_DMAC_DSAR(DMA_SD_RX_CHANNEL) = PHYSADDR(MSC_RXFIFO(MSC_CHN(drive)));    /* DMA source addr */
    if (waligned)
        REG_DMAC_DTAR(DMA_SD_RX_CHANNEL) = PHYSADDR((unsigned long)req->buffer);    /* DMA dest addr */
    else
        REG_DMAC_DTAR(DMA_SD_RX_CHANNEL) = PHYSADDR((unsigned long)&aligned_buffer);    /* DMA dest addr */
    REG_DMAC_DTCR(DMA_SD_RX_CHANNEL) = (size + 3) / 4;    /* DMA transfer count */
    REG_DMAC_DRSR(DMA_SD_RX_CHANNEL) = (drive == SD_SLOT_1) ? DMAC_DRSR_RS_MSC2IN : DMAC_DRSR_RS_MSC1IN;    /* DMA request type */

    REG_DMAC_DCMD(DMA_SD_RX_CHANNEL) =
        DMAC_DCMD_DAI | DMAC_DCMD_SWDH_32 | DMAC_DCMD_DWDH_32 |
        DMAC_DCMD_DS_32BIT;
    REG_DMAC_DCCSR(DMA_SD_RX_CHANNEL) = DMAC_DCCSR_EN | DMAC_DCCSR_NDES;

    /* wait for dma completion */
    while (REG_DMAC_DTCR(DMA_SD_RX_CHANNEL));

    /* clear status and disable channel */
    REG_DMAC_DCCSR(DMA_SD_RX_CHANNEL) = 0;

    if (!waligned)
        memcpy_dma(req->buffer, &aligned_buffer, size, 8);
}

static void jz_sd_transmit_data_dma(const int drive, struct sd_request *req)
{
    unsigned int waligned = (((unsigned int)req->buffer & 0x3) == 0);    /* word aligned ? */
    unsigned int size = req->block_len * req->nob;

    if (!waligned)
    {
        if (size > UNALIGNED_NUM_SECTORS*SD_BLOCK_SIZE)
            size = UNALIGNED_NUM_SECTORS*SD_BLOCK_SIZE;
        memcpy_dma(&aligned_buffer, req->buffer, size, 8);
    }

    /* flush dcache */
    dma_cache_wback_inv((unsigned long) req->buffer, size);
    /* setup dma channel */
    REG_DMAC_DCCSR(DMA_SD_RX_CHANNEL) = 0;
    if (waligned)
        REG_DMAC_DSAR(DMA_SD_TX_CHANNEL) = PHYSADDR((unsigned long) req->buffer);    /* DMA source addr */
    else
        REG_DMAC_DSAR(DMA_SD_RX_CHANNEL) = PHYSADDR((unsigned long)&aligned_buffer);    /* DMA source addr */
    REG_DMAC_DTAR(DMA_SD_TX_CHANNEL) = PHYSADDR(MSC_TXFIFO(MSC_CHN(drive)));    /* DMA dest addr */
    REG_DMAC_DTCR(DMA_SD_TX_CHANNEL) = (size + 3) / 4;    /* DMA transfer count */
    REG_DMAC_DRSR(DMA_SD_TX_CHANNEL) = (drive == SD_SLOT_1) ? DMAC_DRSR_RS_MSC2OUT : DMAC_DRSR_RS_MSC1OUT;    /* DMA request type */

    REG_DMAC_DCMD(DMA_SD_TX_CHANNEL) =
        DMAC_DCMD_SAI | DMAC_DCMD_SWDH_32 | DMAC_DCMD_DWDH_32 |
        DMAC_DCMD_DS_32BIT;
    REG_DMAC_DCCSR(DMA_SD_TX_CHANNEL) = DMAC_DCCSR_EN | DMAC_DCCSR_NDES;

    /* wait for dma completion */
    while (REG_DMAC_DTCR(DMA_SD_TX_CHANNEL));

    /* clear status and disable channel */
    REG_DMAC_DCCSR(DMA_SD_TX_CHANNEL) = 0;
}

void DMA_CALLBACK(DMA_SD_RX_CHANNEL)(void)
{
    if (REG_DMAC_DCCSR(DMA_SD_RX_CHANNEL) & DMAC_DCCSR_AR)
    {
        logf("SD RX DMA address error");
        REG_DMAC_DCCSR(DMA_SD_RX_CHANNEL) &= ~DMAC_DCCSR_AR;
    }

    if (REG_DMAC_DCCSR(DMA_SD_RX_CHANNEL) & DMAC_DCCSR_HLT)
    {
        logf("SD RX DMA halt");
        REG_DMAC_DCCSR(DMA_SD_RX_CHANNEL) &= ~DMAC_DCCSR_HLT;
    }

    if (REG_DMAC_DCCSR(DMA_SD_RX_CHANNEL) & DMAC_DCCSR_TT)
    {
        REG_DMAC_DCCSR(DMA_SD_RX_CHANNEL) &= ~DMAC_DCCSR_TT;
        //sd_rx_dma_callback();
    }
}

void DMA_CALLBACK(DMA_SD_TX_CHANNEL)(void)
{
    if (REG_DMAC_DCCSR(DMA_SD_TX_CHANNEL) & DMAC_DCCSR_AR)
    {
        logf("SD TX DMA address error: %x, %x, %x", var1, var2, var3);
        REG_DMAC_DCCSR(DMA_SD_TX_CHANNEL) &= ~DMAC_DCCSR_AR;
    }

    if (REG_DMAC_DCCSR(DMA_SD_TX_CHANNEL) & DMAC_DCCSR_HLT)
    {
        logf("SD TX DMA halt");
        REG_DMAC_DCCSR(DMA_SD_TX_CHANNEL) &= ~DMAC_DCCSR_HLT;
    }

    if (REG_DMAC_DCCSR(DMA_SD_TX_CHANNEL) & DMAC_DCCSR_TT)
    {
        REG_DMAC_DCCSR(DMA_SD_TX_CHANNEL) &= ~DMAC_DCCSR_TT;
        //sd_tx_dma_callback();
    }
}

#else                /* SD_DMA_ENABLE */

static int jz_sd_receive_data(const int drive, struct sd_request *req)
{
    unsigned int nob = req->nob;
    unsigned int wblocklen = (unsigned int) (req->block_len + 3) >> 2;    /* length in word */
    unsigned char *buf = req->buffer;
    unsigned int *wbuf = (unsigned int *) buf;
    unsigned int waligned = (((unsigned int) buf & 0x3) == 0);    /* word aligned ? */
    unsigned int stat, timeout, data, cnt;

    for (; nob >= 1; nob--)
    {
        timeout = 0x3FFFFFF;

        while (timeout)
        {
            timeout--;
            stat = REG_MSC_STAT(MSC_CHN(drive));

            if (stat & MSC_STAT_TIME_OUT_READ)
                return SD_ERROR_TIMEOUT;
            else if (stat & MSC_STAT_CRC_READ_ERROR)
                return SD_ERROR_CRC;
            else if (!(stat & MSC_STAT_DATA_FIFO_EMPTY)
                 || (stat & MSC_STAT_DATA_FIFO_AFULL))
                /* Ready to read data */
                break;

            udelay(1);
        }

        if (!timeout)
            return SD_ERROR_TIMEOUT;

        /* Read data from RXFIFO. It could be FULL or PARTIAL FULL */
        DEBUG("Receive Data = %d", wblocklen);
        cnt = wblocklen;
        while (cnt)
        {
            data = REG_MSC_RXFIFO(MSC_CHN(drive));
            if (waligned)
                *wbuf++ = data;
            else
            {
                *buf++ = (unsigned char) (data >> 0);
                *buf++ = (unsigned char) (data >> 8);
                *buf++ = (unsigned char) (data >> 16);
                *buf++ = (unsigned char) (data >> 24);
            }
            cnt--;
            while (cnt
                   && (REG_MSC_STAT(MSC_CHN(drive)) &
                   MSC_STAT_DATA_FIFO_EMPTY));
        }
    }

    return SD_NO_ERROR;
}

static int jz_sd_transmit_data(const int drive, struct sd_request *req)
{
    unsigned int nob = req->nob;
    unsigned int wblocklen = (unsigned int) (req->block_len + 3) >> 2;    /* length in word */
    unsigned char *buf = req->buffer;
    unsigned int *wbuf = (unsigned int *) buf;
    unsigned int waligned = (((unsigned int) buf & 0x3) == 0);    /* word aligned ? */
    unsigned int stat, timeout, data, cnt;

    for (; nob >= 1; nob--)
    {
        timeout = 0x3FFFFFF;

        while (timeout)
        {
            timeout--;
            stat = REG_MSC_STAT(MSC_CHN(drive));

            if (stat &
                (MSC_STAT_CRC_WRITE_ERROR |
                 MSC_STAT_CRC_WRITE_ERROR_NOSTS))
                return SD_ERROR_CRC;
            else if (!(stat & MSC_STAT_DATA_FIFO_FULL))
                /* Ready to write data */
                break;

            udelay(1);
        }

        if (!timeout)
            return SD_ERROR_TIMEOUT;

        /* Write data to TXFIFO */
        cnt = wblocklen;
        while (cnt)
        {
            while (REG_MSC_STAT(MSC_CHN(drive)) & MSC_STAT_DATA_FIFO_FULL);

            if (waligned)
                REG_MSC_TXFIFO(MSC_CHN(drive)) = *wbuf++;
            else
            {
                data = *buf++;
                data |= *buf++ << 8;
                data |= *buf++ << 16;
                data |= *buf++ << 24;
                REG_MSC_TXFIFO(MSC_CHN(drive)) = data;
            }

            cnt--;
        }
    }

    return SD_NO_ERROR;
}
#endif

static inline unsigned int jz_sd_calc_clkrt(const int drive, unsigned int rate)
{
    unsigned int clkrt;
    unsigned int clk_src = sd2_0[drive] ? SD_CLOCK_HIGH : SD_CLOCK_FAST;

    clkrt = 0;
    while (rate < clk_src)
    {
        clkrt++;
        clk_src >>= 1;
    }
    return clkrt;
}

static inline void cpm_select_msc_clk(unsigned int rate)
{
    unsigned int div = __cpm_get_pllout2() / rate;

    REG_CPM_MSCCDR = div - 1;
}

/* Set the MMC clock frequency */
static void jz_sd_set_clock(const int drive, unsigned int rate)
{
    int clkrt;

    jz_sd_stop_clock(drive);

    /* select clock source from CPM */
    cpm_select_msc_clk(rate);

    __cpm_enable_pll_change();
    clkrt = jz_sd_calc_clkrt(drive, rate);
    REG_MSC_CLKRT(MSC_CHN(drive)) = clkrt;

    DEBUG("set clock to %u Hz clkrt=%d", rate, clkrt);
}

/********************************************************************************************************************
** Name:      int jz_sd_exec_cmd()
** Function:  send command to the card, and get a response
** Input:     struct sd_request *req: SD request
** Output:    0:  right        >0:  error code
********************************************************************************************************************/
static int jz_sd_exec_cmd(const int drive, struct sd_request *request)
{
    unsigned int cmdat = 0, events = 0;
    int retval, timeout = 0x3fffff;

    /* Indicate we have no result yet */
    request->result = SD_NO_RESPONSE;

    if (request->cmd == SD_CIM_RESET) {
        /* On reset, 1-bit bus width */
        use_4bit[drive] = 0;

        /* Reset MMC/SD controller */
        __msc_reset(MSC_CHN(drive));

        /* On reset, drop SD clock down */
        jz_sd_set_clock(drive, MMC_CLOCK_SLOW);

        /* On reset, stop SD clock */
        jz_sd_stop_clock(drive);
    }
    if (request->cmd == SD_SET_BUS_WIDTH)
    {
        if (request->arg == 0x2)
        {
            DEBUG("Use 4-bit bus width");
            use_4bit[drive] = 1;
        }
        else
        {
            DEBUG("Use 1-bit bus width");
            use_4bit[drive] = 0;
        }
    }

    /* stop clock */
    jz_sd_stop_clock(drive);

    /* mask all interrupts */
    //REG_MSC_IMASK(MSC_CHN(drive)) = 0xffff;
    /* clear status */
    REG_MSC_IREG(MSC_CHN(drive)) = 0xffff;
    /*open interrupt */
    REG_MSC_IMASK(MSC_CHN(drive)) = (~7);
    /* use 4-bit bus width when possible */
    if (use_4bit[drive])
        cmdat |= MSC_CMDAT_BUS_WIDTH_4BIT;

    /* Set command type and events */
    switch (request->cmd)
    {
        /* SD core extra command */
        case SD_CIM_RESET:
            cmdat |= MSC_CMDAT_INIT; /* Initialization sequence sent prior to command */
            break;
            /* bc - broadcast - no response */
        case SD_GO_IDLE_STATE:
        case SD_SET_DSR:
            break;

            /* bcr - broadcast with response */
        case SD_APP_OP_COND:
        case SD_ALL_SEND_CID:
        case SD_GO_IRQ_STATE:
            break;

            /* adtc - addressed with data transfer */
        case SD_READ_DAT_UNTIL_STOP:
        case SD_READ_SINGLE_BLOCK:
        case SD_READ_MULTIPLE_BLOCK:
        case SD_SEND_SCR:
#if SD_DMA_ENABLE
            cmdat |=
                MSC_CMDAT_DATA_EN | MSC_CMDAT_READ | MSC_CMDAT_DMA_EN;
#else
            cmdat |= MSC_CMDAT_DATA_EN | MSC_CMDAT_READ;
#endif
            events = SD_EVENT_RX_DATA_DONE;
            break;

        case 6:
            if (num_6[drive] < 2)
            {
#if SD_DMA_ENABLE
                cmdat |=
                    MSC_CMDAT_DATA_EN | MSC_CMDAT_READ |
                    MSC_CMDAT_DMA_EN;
#else
                cmdat |= MSC_CMDAT_DATA_EN | MSC_CMDAT_READ;
#endif
                events = SD_EVENT_RX_DATA_DONE;
            }
            break;

        case SD_WRITE_DAT_UNTIL_STOP:
        case SD_WRITE_BLOCK:
        case SD_WRITE_MULTIPLE_BLOCK:
        case SD_PROGRAM_CID:
        case SD_PROGRAM_CSD:
        case SD_LOCK_UNLOCK:
#if SD_DMA_ENABLE
            cmdat |=
                MSC_CMDAT_DATA_EN | MSC_CMDAT_WRITE | MSC_CMDAT_DMA_EN;
#else
            cmdat |= MSC_CMDAT_DATA_EN | MSC_CMDAT_WRITE;
#endif
            events = SD_EVENT_TX_DATA_DONE | SD_EVENT_PROG_DONE;
            break;

        case SD_STOP_TRANSMISSION:
            events = SD_EVENT_PROG_DONE;
            break;

        /* ac - no data transfer */
        default:
            break;
    }

    /* Set response type */
    switch (request->rtype)
    {
        case RESPONSE_NONE:
            break;
        case RESPONSE_R1B:
            cmdat |= MSC_CMDAT_BUSY;
            /* FALLTHRU */
        case RESPONSE_R1:
        case RESPONSE_R7:
            cmdat |= MSC_CMDAT_RESPONSE_R1;
            break;
        case RESPONSE_R2_CID:
        case RESPONSE_R2_CSD:
            cmdat |= MSC_CMDAT_RESPONSE_R2;
            break;
        case RESPONSE_R3:
            cmdat |= MSC_CMDAT_RESPONSE_R3;
            break;
        case RESPONSE_R4:
            cmdat |= MSC_CMDAT_RESPONSE_R4;
            break;
        case RESPONSE_R5:
            cmdat |= MSC_CMDAT_RESPONSE_R5;
            break;
        case RESPONSE_R6:
            cmdat |= MSC_CMDAT_RESPONSE_R6;
            break;
        default:
            break;
    }

    /* Set command index */
    if (request->cmd == SD_CIM_RESET)
        REG_MSC_CMD(MSC_CHN(drive)) = SD_GO_IDLE_STATE;
    else
        REG_MSC_CMD(MSC_CHN(drive)) = request->cmd;

    /* Set argument */
    REG_MSC_ARG(MSC_CHN(drive)) = request->arg;

    /* Set block length and nob */
    if (request->cmd == SD_SEND_SCR)
    {    /* get SCR from DataFIFO */
        REG_MSC_BLKLEN(MSC_CHN(drive)) = 8;
        REG_MSC_NOB(MSC_CHN(drive)) = 1;
    }
    else
    {
        REG_MSC_BLKLEN(MSC_CHN(drive)) = request->block_len;
        REG_MSC_NOB(MSC_CHN(drive)) = request->nob;
    }

    /* Set command */
    REG_MSC_CMDAT(MSC_CHN(drive)) = cmdat;

    DEBUG("Send cmd %d cmdat: %x arg: %x resp %d", request->cmd,
          cmdat, request->arg, request->rtype);

    /* Start SD clock and send command to card */
    jz_sd_start_clock(drive);

    /* Wait for command completion */
    //__intc_unmask_irq(IRQ_MSC);
    //semaphore_wait(&sd_wakeup, 100);
    while (timeout-- && !(REG_MSC_STAT(MSC_CHN(drive)) & MSC_STAT_END_CMD_RES));


    if (timeout == 0)
        return SD_ERROR_TIMEOUT;

    REG_MSC_IREG(MSC_CHN(drive)) = MSC_IREG_END_CMD_RES;    /* clear flag */

    /* Check for status */
    retval = jz_sd_check_status(drive, request);
    if (retval)
        return retval;

    /* Complete command with no response */
    if (request->rtype == RESPONSE_NONE)
        return SD_NO_ERROR;

    /* Get response */
    jz_sd_get_response(drive, request);

    /* Start data operation */
    if (events & (SD_EVENT_RX_DATA_DONE | SD_EVENT_TX_DATA_DONE))
    {
        if (events & SD_EVENT_RX_DATA_DONE)
        {
            if (request->cmd == SD_SEND_SCR)
            {
                /* SD card returns SCR register as data. 
                   SD core expect it in the response buffer, 
                   after normal response. */
                request->buffer =
                    (unsigned char *) ((unsigned int) request->response + 5);
            }
#if SD_DMA_ENABLE
            jz_sd_receive_data_dma(drive, request);
#else
            jz_sd_receive_data(drive, request);
#endif
        }

        if (events & SD_EVENT_TX_DATA_DONE)
        {
#if SD_DMA_ENABLE
            jz_sd_transmit_data_dma(drive, request);
#else
            jz_sd_transmit_data(drive, request);
#endif
        }
        //__intc_unmask_irq(IRQ_MSC);
        //semaphore_wait(&sd_wakeup, 100);
        /* Wait for Data Done */
        while (!(REG_MSC_IREG(MSC_CHN(drive)) & MSC_IREG_DATA_TRAN_DONE));
        REG_MSC_IREG(MSC_CHN(drive)) = MSC_IREG_DATA_TRAN_DONE;    /* clear status */
    }

    /* Wait for Prog Done event */
    if (events & SD_EVENT_PROG_DONE)
    {
        //__intc_unmask_irq(IRQ_MSC);
        //semaphore_wait(&sd_wakeup, 100);
        while (!(REG_MSC_IREG(MSC_CHN(drive)) & MSC_IREG_PRG_DONE));
        REG_MSC_IREG(MSC_CHN(drive)) = MSC_IREG_PRG_DONE;    /* clear status */
    }

    /* Command completed */

    return SD_NO_ERROR;    /* return successfully */
}

/*******************************************************************************************************************
** Name:      int sd_chkcard()
** Function:  check whether card is insert entirely
** Input:     NULL
** Output:    1: insert entirely    0: not insert entirely
********************************************************************************************************************/
static int jz_sd_chkcard(const int drive)
{
    return (__gpio_get_pin((drive == SD_SLOT_1) ? PIN_SD1_CD : PIN_SD2_CD) == 0 ? 1 : 0);
}

/* MSC interrupt handler */
void MSC(void)
{
    //semaphore_release(&sd_wakeup);
    logf("MSC interrupt");
}

#ifdef HAVE_HOTSWAP
static void sd_gpio_setup_irq(const int drive, bool inserted)
{
    int pin = (drive == SD_SLOT_1) ? PIN_SD1_CD : PIN_SD2_CD;
    int irq = (drive == SD_SLOT_1) ? IRQ_SD1_CD : IRQ_SD2_CD;
    if(inserted)
        __gpio_as_irq_rise_edge(pin);
    else
        __gpio_as_irq_fall_edge(pin);
    system_enable_irq(irq);
}
#endif

/*******************************************************************************************************************
** Name:      void sd_hardware_init()
** Function:  initialize the hardware condiction that access sd card
** Input:     NULL
** Output:    NULL
********************************************************************************************************************/
static void jz_sd_hardware_init(const int drive)
{
    if (drive == SD_SLOT_1)
        __cpm_start_msc2();   /* enable mmc2 clock */
    else
        __cpm_start_msc1();   /* enable mmc1 clock */
#ifdef HAVE_HOTSWAP
    sd_gpio_setup_irq(drive, jz_sd_chkcard(drive));
#endif
    __msc_reset(MSC_CHN(drive));  /* reset mmc/sd controller */
    SD_IRQ_MASK(MSC_CHN(drive));  /* mask all IRQs */
    jz_sd_stop_clock(drive); /* stop SD clock */
}

static int sd_send_cmd(const int drive, struct sd_request *request, int cmd, unsigned int arg,
                         unsigned short nob, unsigned short block_len,
                         enum sd_rsp_t rtype, unsigned char* buffer)
{
    request->cmd = cmd;
    request->arg = arg;
    request->rtype = rtype;
    request->nob = nob;
    request->block_len = block_len;
    request->buffer = buffer;
    request->cnt = nob * block_len;

    return jz_sd_exec_cmd(drive, request);
}

static void sd_simple_cmd(const int drive, struct sd_request *request, int cmd, unsigned int arg,
                           enum sd_rsp_t rtype)
{
    sd_send_cmd(drive, request, cmd, arg, 0, 0, rtype, NULL);
}

#define SD_INIT_DOING   0
#define SD_INIT_PASSED  1
#define SD_INIT_FAILED  2
static int sd_init_card_state(const int drive, struct sd_request *request)
{
    struct sd_response_r1 r1;
    struct sd_response_r3 r3;
    int retval, i, ocr = 0x40300000, limit_41 = 0;

    switch (request->cmd)
    {
        case SD_GO_IDLE_STATE: /* No response to parse */
            sd_simple_cmd(drive, request, SD_SEND_IF_COND, 0x1AA, RESPONSE_R1);
            break;

        case SD_SEND_IF_COND:
            retval = sd_unpack_r1(request, &r1);
            sd_simple_cmd(drive, request, SD_APP_CMD,  0, RESPONSE_R1);
            break;

        case SD_APP_CMD:
            retval = sd_unpack_r1(request, &r1);
            if (retval & (limit_41 < 100))
            {
                DEBUG("sd_init_card_state: unable to SD_APP_CMD error=%d", 
                      retval);
                limit_41++;
                sd_simple_cmd(drive, request, SD_APP_OP_COND, ocr, RESPONSE_R3);
            }
            else if (limit_41 < 100)
            {
                limit_41++;
                sd_simple_cmd(drive, request, SD_APP_OP_COND, ocr, RESPONSE_R3);
            }
            else
                /* reset the card to idle*/
                sd_simple_cmd(drive, request, SD_GO_IDLE_STATE, 0, RESPONSE_NONE);
            break;

        case SD_APP_OP_COND:
            retval = sd_unpack_r3(request, &r3);
            if (retval)
                break;

            DEBUG("sd_init_card_state: read ocr value = 0x%08x", r3.ocr);
            card[drive].ocr = r3.ocr;

            if(!(r3.ocr & SD_CARD_BUSY || ocr == 0))
            {
                sleep(HZ / 100);
                sd_simple_cmd(drive, request, SD_APP_CMD, 0, RESPONSE_R1);
            }
            else
            {
                /* Set the data bus width to 4 bits */
                use_4bit[drive] = 1;
                sd_simple_cmd(drive, request, SD_ALL_SEND_CID, 0, RESPONSE_R2_CID);
            }
            break;

        case SD_ALL_SEND_CID:
            for(i=0; i<4; i++)
                card[drive].cid[i] = ((request->response[1+i*4]<<24) | (request->response[2+i*4]<<16) | 
                               (request->response[3+i*4]<< 8) | request->response[4+i*4]);

            logf("CID: %08lx%08lx%08lx%08lx", card[drive].cid[0], card[drive].cid[1], card[drive].cid[2], card[drive].cid[3]);
            sd_simple_cmd(drive, request, SD_SEND_RELATIVE_ADDR, 0, RESPONSE_R6);
            break;
        case SD_SEND_RELATIVE_ADDR:
            retval = sd_unpack_r6(request, &r1, &card[drive].rca);
            card[drive].rca = card[drive].rca << 16; 
            DEBUG("sd_init_card_state: Get RCA from SD: 0x%04lx Status: %x", card[drive].rca, r1.status);
            if (retval)
            {
                DEBUG("sd_init_card_state: unable to SET_RELATIVE_ADDR error=%d", 
                      retval);
                return SD_INIT_FAILED;
            }

            sd_simple_cmd(drive, request, SD_SEND_CSD, card[drive].rca, RESPONSE_R2_CSD);
            break;

        case SD_SEND_CSD:
            for(i=0; i<4; i++)
                card[drive].csd[i] = ((request->response[1+i*4]<<24) | (request->response[2+i*4]<<16) | 
                               (request->response[3+i*4]<< 8) | request->response[4+i*4]);

            sd_parse_csd(&card[drive]);
            sd2_0[drive] = (card_extract_bits(card[drive].csd, 127, 2) == 1);

            logf("CSD: %08lx%08lx%08lx%08lx", card[drive].csd[0], card[drive].csd[1], card[drive].csd[2], card[drive].csd[3]);
            DEBUG("SD card is ready");
            jz_sd_set_clock(drive, SD_CLOCK_FAST);
            return SD_INIT_PASSED;

        default:
            DEBUG("sd_init_card_state: error!  Illegal last cmd %d", request->cmd);
            return SD_INIT_FAILED;
    }

    return SD_INIT_DOING;
}

static int sd_switch(const int drive, struct sd_request *request, int mode, int group,
              unsigned char value, unsigned char * resp)
{
    unsigned int arg;

    mode = !!mode;
    value &= 0xF;
    arg = (mode << 31 | 0x00FFFFFF);
    arg &= ~(0xF << (group * 4));
    arg |= value << (group * 4);
    sd_send_cmd(drive, request, 6, arg, 1, 64, RESPONSE_R1, resp);

    return 0;
}

/*
 * Fetches and decodes switch information
 */
static int sd_read_switch(const int drive, struct sd_request *request)
{
    unsigned int status[64 / 4];

    memset((unsigned char *)status, 0, 64);
    sd_switch(drive, request, 0, 0, 1, (unsigned char*) status);

    if (((unsigned char *)status)[13] & 0x02)
        return 0;
    else 
        return 1;
}

/*
 * Test if the card supports high-speed mode and, if so, switch to it.
 */
static int sd_switch_hs(const int drive, struct sd_request *request)
{
    unsigned int status[64 / 4];

    sd_switch(drive, request, 1, 0, 1, (unsigned char*) status);
    return 0;
}

static int sd_select_card(const int drive)
{
    struct sd_request request;
    struct sd_response_r1 r1;
    int retval;

    sd_simple_cmd(drive, &request, SD_SELECT_CARD, card[drive].rca,
               RESPONSE_R1B);
    retval = sd_unpack_r1(&request, &r1);
    if (retval)
        return retval;

    if (sd2_0[drive])
    {
        retval = sd_read_switch(drive, &request);
        if (!retval)
        {
            sd_switch_hs(drive, &request);
            jz_sd_set_clock(drive, SD_CLOCK_HIGH);
        }
    }
    num_6[drive] = 3;
    sd_simple_cmd(drive, &request, SD_APP_CMD, card[drive].rca,
               RESPONSE_R1);
    retval = sd_unpack_r1(&request, &r1);
    if (retval)
        return retval;
    sd_simple_cmd(drive, &request, SD_SET_BUS_WIDTH, 2, RESPONSE_R1);
    retval = sd_unpack_r1(&request, &r1);
    if (retval)
        return retval;

    card[drive].initialized = 1;

    return 0;
}

static int sd_init_device(const int drive)
{
    int retval = 0;
    struct sd_request init_req;
    register int timeout = 1000;

    mutex_lock(&sd_mtx);

    /* Initialise card data as blank */
    memset(&card[drive], 0, sizeof(tCardInfo));

    sd2_0[drive] = 0;
    num_6[drive] = 0;
    use_4bit[drive] = 0;

    /* reset mmc/sd controller */
    jz_sd_hardware_init(drive);

    sd_simple_cmd(drive, &init_req, SD_CIM_RESET,     0,     RESPONSE_NONE);
    sd_simple_cmd(drive, &init_req, SD_GO_IDLE_STATE, 0,     RESPONSE_NONE);

    sleep(HZ/2); /* Give the card/controller some rest */

    while(timeout-- && ((retval = sd_init_card_state(drive, &init_req)) == SD_INIT_DOING));
    retval = (retval == SD_INIT_PASSED ? sd_select_card(drive) : -1);

    if (drive == SD_SLOT_1)
        __cpm_stop_msc2(); /* disable SD1 clock */
    else
        __cpm_stop_msc1(); /* disable SD2 clock */

    mutex_unlock(&sd_mtx);

    return retval;
}

int sd_init(void)
{
    static bool inited = false;

    sd_init_gpio();     /* init GPIO */

#if SD_DMA_ENABLE
    __dmac_channel_enable_clk(DMA_SD_RX_CHANNEL);
    __dmac_channel_enable_clk(DMA_SD_TX_CHANNEL);
#endif

    if(!inited)
    {
        semaphore_init(&sd_wakeup, 1, 0);
        mutex_init(&sd_mtx);
        queue_init(&sd_queue, true);
        create_thread(sd_thread, sd_stack, sizeof(sd_stack), 0,
                      sd_thread_name IF_PRIO(, PRIORITY_USER_INTERFACE)
                      IF_COP(, CPU));

        inited = true;
    }

    for (int drive = 0; drive < NUM_DRIVES; drive++)
        sd_init_device(drive);

    return 0;
}

static inline bool card_detect_target(const int drive)
{
    return (jz_sd_chkcard(drive) == 1);
}

tCardInfo* card_get_info_target(const int drive)
{
    return &card[drive];
}

static inline void sd_start_transfer(const int drive)
{
    mutex_lock(&sd_mtx);
    if (drive == SD_SLOT_1)
        __cpm_start_msc2();
    else
        __cpm_start_msc1();
    led(true);
}

static inline void sd_stop_transfer(const int drive)
{
    led(false);
    if (drive == SD_SLOT_1)
        __cpm_stop_msc2();
    else
        __cpm_stop_msc1();
    mutex_unlock(&sd_mtx);
}

int sd_read_sectors(const int drive, unsigned long start, int count, void* buf)
{
    sd_start_transfer(drive);

    struct sd_request request;
    struct sd_response_r1 r1;
    int retval = -1;

    if (!card_detect_target(drive) || count == 0 || start > card[drive].numblocks)
        goto err;

    if(card[drive].initialized == 0 && !sd_init_device(drive))
        goto err;

    sd_simple_cmd(drive, &request, SD_SEND_STATUS, card[drive].rca, RESPONSE_R1);
    retval = sd_unpack_r1(&request, &r1);
    if (retval && (retval != SD_ERROR_STATE_MISMATCH))
        goto err;

    sd_simple_cmd(drive, &request, SD_SET_BLOCKLEN, SD_BLOCK_SIZE, RESPONSE_R1);
    if ((retval = sd_unpack_r1(&request, &r1)))
        goto err;

    if (sd2_0[drive])
    {
        sd_send_cmd(drive, &request, SD_READ_MULTIPLE_BLOCK, start,
                     count, SD_BLOCK_SIZE, RESPONSE_R1, buf);
        if ((retval = sd_unpack_r1(&request, &r1)))
            goto err;
    }
    else
    {
        sd_send_cmd(drive, &request, SD_READ_MULTIPLE_BLOCK,
                     start * SD_BLOCK_SIZE, count,
                     SD_BLOCK_SIZE, RESPONSE_R1, buf);
        if ((retval = sd_unpack_r1(&request, &r1)))
            goto err;
    }

    last_disk_activity = current_tick;

    sd_simple_cmd(drive, &request, SD_STOP_TRANSMISSION, 0, RESPONSE_R1B);
    if ((retval = sd_unpack_r1(&request, &r1)))
        goto err;

err:
    sd_stop_transfer(drive);

    return retval;
}

int sd_write_sectors(const int drive, unsigned long start, int count, const void* buf)
{
    sd_start_transfer(drive);

    struct sd_request request;
    struct sd_response_r1 r1;
    int retval = -1;

    if (!card_detect_target(drive) || count == 0 || start > card[drive].numblocks)
        goto err;

    if(card[drive].initialized == 0 && !sd_init_device(drive))
        goto err;

    sd_simple_cmd(drive, &request, SD_SEND_STATUS, card[drive].rca, RESPONSE_R1);
    retval = sd_unpack_r1(&request, &r1);
    if (retval && (retval != SD_ERROR_STATE_MISMATCH))
        goto err;

    sd_simple_cmd(drive, &request, SD_SET_BLOCKLEN, SD_BLOCK_SIZE, RESPONSE_R1);
    if ((retval = sd_unpack_r1(&request, &r1)))
        goto err;

    if (sd2_0[drive])
    {
        sd_send_cmd(drive, &request, SD_WRITE_MULTIPLE_BLOCK, start,
                 count, SD_BLOCK_SIZE, RESPONSE_R1,
                 (void*)buf);
        if ((retval = sd_unpack_r1(&request, &r1)))
            goto err;
    }
    else
    {
        sd_send_cmd(drive, &request, SD_WRITE_MULTIPLE_BLOCK,
                 start * SD_BLOCK_SIZE, count,
                 SD_BLOCK_SIZE, RESPONSE_R1, (void*)buf);
        if ((retval = sd_unpack_r1(&request, &r1)))
            goto err;
    }

    last_disk_activity = current_tick;

    sd_simple_cmd(drive, &request, SD_STOP_TRANSMISSION, 0, RESPONSE_R1B);
    if ((retval = sd_unpack_r1(&request, &r1)))
        goto err;

err:
    sd_stop_transfer(drive);

    return retval;
}

long sd_last_disk_activity(void)
{
    return last_disk_activity;
}

int sd_spinup_time(void)
{
    return 0;
}

void sd_enable(bool on)
{
    (void)on;
}

void sd_sleepnow(void)
{
}

bool sd_disk_is_active(void)
{
    return false;
}

int sd_soft_reset(void)
{
    return 0;
}

#ifdef HAVE_HOTSWAP
bool sd_removable(const int drive)
{
    (void)drive;
    return true;
}

static int sd1_oneshot_callback(struct timeout *tmo)
{
    (void)tmo;
    int state = card_detect_target(SD_SLOT_1);

    /* This is called only if the state was stable for 300ms - check state
     * and post appropriate event. */
    if (state)
        queue_broadcast(SYS_HOTSWAP_INSERTED, 0);
    else
        queue_broadcast(SYS_HOTSWAP_EXTRACTED, 0);

    sd_gpio_setup_irq(SD_SLOT_1, state);

    return 0;
}

static int sd2_oneshot_callback(struct timeout *tmo)
{
    (void)tmo;
    int state = card_detect_target(SD_SLOT_2);

    /* This is called only if the state was stable for 300ms - check state
     * and post appropriate event. */
    if (state)
        queue_broadcast(SYS_HOTSWAP_INSERTED, 1);
    else
        queue_broadcast(SYS_HOTSWAP_EXTRACTED, 1);

    sd_gpio_setup_irq(SD_SLOT_2, state);

    return 0;
}

/* called on insertion/removal interrupt */
void GPIO_SD1_CD(void)
{
    static struct timeout sd1_oneshot;
    timeout_register(&sd1_oneshot, sd1_oneshot_callback, (3*HZ/10), 0);
}

void GPIO_SD2_CD(void)
{
    static struct timeout sd2_oneshot;
    timeout_register(&sd2_oneshot, sd2_oneshot_callback, (3*HZ/10), 0);
}
#endif

bool sd_present(const int drive)
{
    return card_detect_target(drive);
}

#ifdef CONFIG_STORAGE_MULTI
int sd_num_drives(int first_drive)
{
    sd_drive_nr = first_drive;
    return NUM_DRIVES;
}
#endif /* CONFIG_STORAGE_MULTI */

static void sd_thread(void)
{
    struct queue_event ev;
    bool idle_notified = false;

    while (1)
    {
        queue_wait_w_tmo(&sd_queue, &ev, HZ);

        switch (ev.id)
        {
#ifdef HAVE_HOTSWAP
        case SYS_HOTSWAP_INSERTED:
        case SYS_HOTSWAP_EXTRACTED:;
            int microsd_init = ev.id == SYS_HOTSWAP_INSERTED ? 0 : 1;

            /* We now have exclusive control of fat cache and sd.
             * Release "by force", ensure file
             * descriptors aren't leaked and any busy
             * ones are invalid if mounting. */
            disk_unmount(sd_drive_nr+ev.data); /* release "by force" */

            mutex_lock(&sd_mtx); /* lock-out card activity */

            /* Force card init for new card, re-init for re-inserted one or
             * clear if the last attempt to init failed with an error. */
            card[ev.data].initialized = 0;

            if(ev.id == SYS_HOTSWAP_INSERTED)
                sd_init_device(ev.data);
                
            mutex_unlock(&sd_mtx);

            if (jz_sd_chkcard(ev.data) == 1)
                microsd_init += disk_mount(sd_drive_nr+ev.data); /* 0 if fail */

            /* Access is now safe */
           /*
            * One or more mounts succeeded, or this was an EXTRACTED event,
            * in both cases notify the system about the changed filesystems
            */
            if(microsd_init)
                queue_broadcast(SYS_FS_CHANGED, 0);

            break;
#endif /* HAVE_HOTSWAP */
        case SYS_TIMEOUT:
            if (TIME_BEFORE(current_tick, last_disk_activity+(3*HZ)))
                idle_notified = false;
            else
            {
                if (!idle_notified)
                {
                    call_storage_idle_notifys(false);
                    idle_notified = true;
                }
            }
            break;
        case SYS_USB_CONNECTED:
            usb_acknowledge(SYS_USB_CONNECTED_ACK);
            /* Wait until the USB cable is extracted again */
            usb_wait_for_disconnect(&sd_queue);
            break;
        }
    }
}
