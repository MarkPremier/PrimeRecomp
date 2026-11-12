/* Guest address-space layout. Must match tools/pef.py. */
#pragma once

#define PT_LOWMEM_BASE        0x00000000u
#define PT_LOWMEM_SIZE        0x00010000u

#define PT_CODE_BASE          0x01000000u
#define PT_DATA_BASE          0x01100000u
#define PT_IMPORT_TVEC_BASE   0x01200000u   /* 8 bytes per import: {code, toc} */
#define PT_IMPORT_CODE_BASE   0x01300000u   /* 4 bytes per import: magic code addresses */
#define PT_IMPORT_DATA_BASE   0x01400000u   /* 256 bytes per imported data symbol */
#define PT_IMAGE_REGION_END   0x01500000u

#define PT_HEAP_BASE          0x02000000u   /* Memory Manager application heap */
#define PT_HEAP_END           0x0A000000u   /* 128 MB */

#define PT_SYSTEM_BASE        0x0C000000u   /* runtime-owned guest objects (ports, devices, ...) */
#define PT_SYSTEM_END         0x0E000000u

#define PT_STACK_BASE         0x0F000000u
#define PT_STACK_TOP          0x0F100000u   /* 1 MB guest stack */
