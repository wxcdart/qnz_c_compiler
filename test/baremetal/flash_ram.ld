/*
 * flash_ram.ld - Standard baremetal memory layout test script.
 * Defines FLASH (ROM) and RAM regions with LMA/VMA separation.
 */

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 128K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 32K
}

ENTRY(Reset_Handler)

_stack_top = ORIGIN(RAM) + LENGTH(RAM);
