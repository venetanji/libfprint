/*
 * OMS Microelectronics Match-On-Chip fingerprint driver for libfprint
 * Copyright (C) 2026 Liu Sen
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#pragma once

#include "fpi-device.h"
#include "fpi-ssm.h"

G_DECLARE_FINAL_TYPE (FpiDeviceOmsMoc, fpi_device_omsmoc, FPI, DEVICE_OMSMOC, FpDevice)

/* ------------------------------------------------------------------ *
* Device information                                                  *
* ------------------------------------------------------------------ */

#define OMSMOC_DRIVER_FULLNAME "Jiangxi OMS Microelectronics MOC Sensors"

/* USB endpoints (bulk). */
#define OMSMOC_EP_OUT 0x01
#define OMSMOC_EP_IN 0x81

/* ------------------------------------------------------------------ *
* State machine descriptions                                          *
*                                                                     *
* The driver uses the following nested state machines:                *
*                                                                     *
*  - One "logical I/O" state machine (OmsIoState) is instantiated per *
*    USB transaction.  Each logical read/write is internally split    *
*    into three physical USB transfers (see the documentation of the  *
*    OmsIoState enum).                                                *
*                                                                     *
*  - One "task" state machine per high level operation (enroll,       *
*    identify, list, delete, clear_storage).  Each task state either  *
*    launches one logical I/O (and is advanced when it completes) or  *
*    performs a decision / finalization.                              *
*                                                                     *
*  The nesting works as follows: while a task state machine is        *
*  running, starting a logical I/O creates a transient child          *
*  state machine.  When that child completes the task machine is      *
*  woken up again (fpi_ssm_next_state()/fpi_ssm_jump_to_state()) and  *
*  the received frame is interpreted at the entry of the following    *
*  task state.                                                        *
* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ *
* Logical I/O machine                                                 *
* ------------------------------------------------------------------ */

typedef enum {
  /* Logical write: OUT the 8 byte USB envelope, OUT the 64 byte
   * command frame, then IN the fixed "USBS" trailer. */
  /* Logical read : OUT the 8 byte USB envelope, IN  the response
   * frame, then IN the fixed "USBS" trailer. */
  OMS_IO_ENVELOPE = 0,
  OMS_IO_PAYLOAD,
  OMS_IO_TRAILER,
  OMS_IO_NUM_STATES,
} OmsIoState;

/* ------------------------------------------------------------------ *
* Task machines                                                       *
* ------------------------------------------------------------------ *
* NOTE on the polling states (DUP_READ / IDENTIFY_READ / ENROLL_READ):
* complex instructions do not have a fixed number of logical reads.
* E.g. AutoIdentify may skip the "image captured" stage when there are
* no templates on the module, and it may answer "not ready" (a frame of
* zeros) when polled faster than it can produce the next status.
*
* Therefore, after a write each *_READ state performs ONE logical read
* and the following *_DECIDE state inspects the reply:
*
*   - the reply is a valid *intermediate* status  -> jump back to *_READ
*   - the reply is the *terminal* result          -> act / continue
*   - the reply is garbage / not ready            -> (handled inside the
*                                                     transport layer by
*                                                     repeating the read)
*
* The task machines below no longer assume a fixed number of reads.
* ------------------------------------------------------------------ */

typedef enum {
  OMS_ENROLL_DUP_SEND = 0,   /* AutoIdentify (duplicate check): write   */
  OMS_ENROLL_DUP_READ,       /* ... poll reads until a terminal result  */
  OMS_ENROLL_DUP_DECIDE,
  OMS_ENROLL_TABLE1_SEND,    /* ReadIndexTable #1                       */
  OMS_ENROLL_TABLE1_RECV,
  OMS_ENROLL_TABLE2_SEND,    /* ReadIndexTable #2 (stable snapshot)     */
  OMS_ENROLL_TABLE2_RECV,
  OMS_ENROLL_ENROLL_SEND,    /* AutoEnrollCmd: write                    */
  OMS_ENROLL_ENROLL_READ,    /* ... poll reads until "store result"     */
  OMS_ENROLL_ENROLL_DECIDE,
  OMS_ENROLL_VERIFY_NUM_SEND,  /* GetValidTemplateNum: write           */
  OMS_ENROLL_VERIFY_NUM_RECV,
  OMS_ENROLL_VERIFY_TABLE_SEND,/* ReadIndexTable: write                 */
  OMS_ENROLL_VERIFY_TABLE_RECV,
  OMS_ENROLL_NUM_STATES,
} OmsEnrollState;

typedef enum {
  OMS_IDENTIFY_SEND = 0,     /* AutoIdentify: write                     */
  OMS_IDENTIFY_READ,         /* ... poll reads until a terminal result  */
  OMS_IDENTIFY_DECIDE,
  OMS_IDENTIFY_NUM_STATES,
} OmsIdentifyState;

typedef enum {
  OMS_LIST_SEND = 0,         /* ReadIndexTable: send command          */
  OMS_LIST_RECV,             /* ... read the bitmap                   */
  OMS_LIST_NUM_STATES,
} OmsListState;

typedef enum {
  OMS_DELETE_SEND = 0,       /* DeleteChar: send command              */
  OMS_DELETE_RECV,           /* ... read confirmation                 */
  OMS_DELETE_NUM_STATES,
} OmsDeleteState;

typedef enum {
  OMS_CLEAR_QUERY_SEND = 0,  /* ReadIndexTable to snapshot the IDs    */
  OMS_CLEAR_QUERY_RECV,
  OMS_CLEAR_DELETE_SEND,     /* DeleteChar for the current ID         */
  OMS_CLEAR_DELETE_RECV,
  OMS_CLEAR_DELETE_NEXT,     /* decision: more IDs left?              */
  OMS_CLEAR_NUM_STATES,
} OmsClearState;

typedef enum {
  OMS_CANCEL_SEND = 0,       /* OMSCancel: send command               */
  OMS_CANCEL_READ,           /* ... read confirmation                 */
  OMS_CANCEL_NUM_STATES,
} OmsCancelState;
