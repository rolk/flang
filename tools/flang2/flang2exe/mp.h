/*
 * Copyright (c) 2017, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

/* mp.h  -  various OpenMP definitions */

#ifndef __MP_H__
#define __MP_H__

/* Bit-maskable OpenMP Tasking Flags */
#define MP_TASK_UNTIED 0x01
#define MP_TASK_IF 0x02
#define MP_TASK_ORPHANED 0x04
#define MP_TASK_NESTED 0x08
#define MP_TASK_FORCED_DEFER 0x10
#define MP_TASK_FINAL 0x20
#define MP_TASK_IMMEDIATE 0x40
#define MP_TASK_MERGEABLE 0x80
#define MP_TASK_MERGEABLE 0x80
#define MP_TASK_PRIORITY 0x100

#define MP_TASK_NOGROUP 0x1000
#define MP_TASK_GRAINSIZE 0x2000
#define MP_TASK_NUM_TASKS 0x4000

/* Schedule attributes for MP_SCH_
 * These are used to represent the MP_XXX for C or DI_XXX for FTN
 *
 * Basic type of schedule (auto, static, dynamic, guided, etc.)
 * are represented by the low byte
 */
#define MP_SCH_TYPE_MASK 0x000000FF
#define MP_SCH_STATIC 0x0
#define MP_SCH_DYNAMIC 0x1
#define MP_SCH_GUIDED 0x2
#define MP_SCH_INTERLEAVE 0x3
#define MP_SCH_RUNTIME 0x4
#define MP_SCH_AUTO 0x5
#define MP_SCH_DIST_STATIC 0x6       /* use in distribute parallel for */
#define MP_SCH_DIST_DYNAMIC 0x7      /* use in distribute parallel for */

/* The second byte represents special case flags for static (maskable) */
#define MP_SCH_SPC_MASK 0x0000FF00
#define MP_SCH_SPC_SHIFT 8
#define MP_SCH_CHUNK_1 0x00000100 /* Chunk == 1 (static cyclic) */
#define MP_SCH_BLK_CYC 0x00000200 /* Chunk > 1  (block cyclic)  */
#define MP_SCH_BLK_ALN 0x00000400 /* Static block aligned       */

/* The high (third) byte represents attributes (maskable) */
#define MP_SCH_ATTR_MASK 0x00FF0000
#define MP_SCH_ATTR_SHIFT 16
#define MP_SCH_ATTR_ORDERED 0x00010000 /* Ordered */
#define MP_SCH_ATTR_CHUNKED 0x00020000 /* Chunked */
#define MP_SCH_ATTR_DIST 0x00040000    /* distributed */

/* Target/Target combine attribute */
#define MP_TGT_NOWAIT 0x01   /* if NOWAIT is present */
#define MP_TGT_IFTARGET 0x02 /* IF(target)   clause is present */
#define MP_TGT_IFPAR 0x04    /* IF(parallel) clause is present */
#define MP_TGT_DEPEND_IN                               \
  0x08 /* depend is present and has dependence-type IN \
          */
#define MP_TGT_DEPEND_OUT \
  0x10 /* depend is present and has dependence-type OUT */
#define MP_TGT_DEPEND_INOUT \
  0x20 /* Depend is present and has dependence-type INOUT */

typedef enum omp_proc_bind_t {
    MP_PROC_BIND_FALSE = 0,
    MP_PROC_BIND_TRUE,
    MP_PROC_BIND_MASTER,
    MP_PROC_BIND_CLOSE,
    MP_PROC_BIND_SPREAD,
} omp_proc_bind_t;

#endif /* __MP_H__ */
