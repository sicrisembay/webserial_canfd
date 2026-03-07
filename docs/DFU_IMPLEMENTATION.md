# USB DFU Implementation Outline

## Overview

This document outlines the steps required to add USB Device Firmware Upgrade (DFU) support to the WebSerial CAN-FD bridge firmware running on the STM32G431C8TX.

The STM32G431C8TX has:
- **128 KB Flash** at `0x08000000`
- **32 KB RAM** at `0x20000000`
- A **built-in ROM bootloader** (system memory at `0x1FFF0000`) that supports USB DFU natively

Two approaches are described. The recommended path is **Approach A** (jump to ROM bootloader) because it is simpler, does not consume application flash space, and reuses ST's validated DFU stack. **Approach B** (custom bootloader in flash) is described as an alternative when more control over the upgrade process is needed.

---

## Approach A: Jump to STM32 ROM Bootloader (Recommended)

The STM32G431 contains a factory-programmed bootloader in system memory that enumerates as a USB DFU device (`idVendor=0x0483`, `idProduct=0xDF11`). The application only needs to trigger a jump into it.

### A1. Define a DFU Entry Trigger

Choose how the application decides to enter DFU mode:

- **Option 1 – Magic word in RAM:** Before resetting, the application writes a known 32-bit pattern (e.g., `0xDEADBEEF`) to a specific RAM address marked `__attribute__((section(".noinit")))`. On next boot, the startup code checks this address before `main()` is called.
- **Option 2 – GPIO / button:** Sample a dedicated pin (e.g., reuse `WORD_LED_Pin` if it can be reconfigured as input at startup) in early boot. If asserted, enter DFU.
- **Option 3 – Firmware command via USB CDC frame:** Add a new payload command code to the existing frame protocol (see `FRAME_SPECIFICATION.md`). When received, the firmware triggers a reset into DFU mode.

Option 3 is the most user-friendly because it requires no extra hardware and integrates cleanly with the existing WebSerial host software.

### A2. Add a CDC Command to Request DFU

1. Define a new command code (e.g., `CMD_ENTER_DFU = 0xF0`) in `frameParser.h` / `canParser.h`.
2. In `frameParser.c`, handle the new command: write the magic word to the `.noinit` RAM variable and call `NVIC_SystemReset()`.

### A3. Implement the ROM Bootloader Jump

Add a function (e.g., in `main.c` or a new `dfu.c`) that is called early in startup before `HAL_Init()`:

```c
#define DFU_MAGIC_WORD  0xDEADBEEF
#define DFU_RAM_ADDRESS 0x2000FF00   /* top of RAM, adjust to avoid collision */

/* Place this in a .noinit section so it survives a software reset */
__attribute__((section(".noinit"))) volatile uint32_t dfu_flag;

void JumpToBootloader(void)
{
    /* Disable all interrupts and peripherals */
    __disable_irq();
    HAL_DeInit();
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL  = 0;

    /* Remap to system memory */
    __HAL_SYSCFG_REMAPMEMORY_SYSTEMFLASH();

    /* Set stack pointer and jump */
    uint32_t bootloader_address = 0x1FFF0000;
    typedef void (*pFunction)(void);
    uint32_t *bootloader_msp  = (uint32_t *) bootloader_address;
    pFunction bootloader_jump = (pFunction) *(uint32_t *)(bootloader_address + 4);

    __set_MSP(*bootloader_msp);
    bootloader_jump();
}
```

Call `JumpToBootloader()` at the very top of `main()` (before any peripheral init) after checking the magic word:

```c
int main(void)
{
    if (dfu_flag == DFU_MAGIC_WORD) {
        dfu_flag = 0;
        JumpToBootloader();
    }
    /* ... normal init continues ... */
}
```

### A4. Reserve the `.noinit` RAM Section in the Linker Script

In `STM32G431C8TX_FLASH.ld`, add a section that is never initialized by the startup code:

```ld
.noinit (NOLOAD) :
{
    . = ALIGN(4);
    *(.noinit)
    . = ALIGN(4);
} >RAM
```

Add this section before the heap section so it does not overlap the stack.

### A5. USB Clock Requirement for ROM Bootloader

The ROM bootloader requires the USB clock to be running when it takes control. Ensure HSI48 is enabled and CRS (Clock Recovery System) is active before calling `JumpToBootloader()`. The current `SystemClock_Config()` already enables `HSI48`; if the jump happens before `SystemClock_Config()`, call a minimal clock setup function first or add the HSI48 enable directly in `JumpToBootloader()`:

```c
/* Ensure HSI48 is on for USB */
RCC->CRRCR |= RCC_CRRCR_HSI48ON;
while (!(RCC->CRRCR & RCC_CRRCR_HSI48RDY));
```

### A6. Host-Side Flashing Workflow

1. Send the `CMD_ENTER_DFU` frame over the existing WebSerial CDC connection.
2. The device resets and re-enumerates as a DFU device.
3. Flash the new firmware using **dfu-util** or **STM32CubeProgrammer**:

```bash
# Using dfu-util
dfu-util -a 0 -s 0x08000000:leave -D firmware.bin

# Using STM32CubeProgrammer CLI
STM32_Programmer_CLI -c port=USB1 -d firmware.bin 0x08000000 -v -g 0x08000000
```

4. The device resets and runs the new firmware.

---

## Approach B: Custom USB DFU Bootloader in Flash

Use this approach if you need to customize the upgrade process (e.g., firmware signing, progress feedback via LEDs, fallback/rollback support).

### B1. Plan the Flash Layout

With a 16 KB bootloader, the flash is partitioned as follows:

| Region       | Start Address  | Size    |
|--------------|----------------|---------|
| Bootloader   | `0x08000000`   | 16 KB   |
| Application  | `0x08004000`   | 112 KB  |

> **Note:** The STM32G431C8TX erase granularity is **2 KB per page**. Choose a bootloader size that is a multiple of 2 KB.

### B2. Create the Bootloader Project

Create a separate STM32CubeIDE project for the bootloader:

1. Add the **USB Device** middleware with the **DFU** class (`usbd_dfu.c`, `usbd_dfu_flash_if.c`).
2. Configure the linker script so that the bootloader occupies only `0x08000000–0x08003FFF` (16 KB).
3. Implement the DFU media interface (`USBD_DFU_MediaTypeDef`) to read/erase/write application flash pages (`0x08004000` and above).
4. Implement an entry condition check (magic word in `.noinit` RAM, or GPIO).
5. If no DFU entry condition is met, jump to the application:

```c
void JumpToApplication(void)
{
    uint32_t app_address = 0x08004000;
    uint32_t *app_vector = (uint32_t *) app_address;

    /* Check if the application is present (stack pointer in RAM range) */
    if ((app_vector[0] & 0x2FFE0000) != 0x20000000) return;

    __disable_irq();
    SCB->VTOR = app_address;
    __set_MSP(app_vector[0]);
    ((void (*)(void)) app_vector[1])();
}
```

### B3. Modify the Application Project

1. **Linker script** – Shift FLASH origin to `0x08004000` and reduce length to `112K`:

```ld
MEMORY
{
  RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 32K
  FLASH  (rx) : ORIGIN = 0x08004000, LENGTH = 112K
}
```

2. **Vector Table Offset** – In `system_stm32g4xx.c`, set `VTOR` to the application base:

```c
SCB->VTOR = 0x08004000;
```

Or define `VECT_TAB_OFFSET` in `system_stm32g4xx.c`:

```c
#define VECT_TAB_OFFSET  0x4000U
```

3. **Generate a binary** – The flash tool must target `0x08004000`, not `0x08000000`.

### B4. LED Feedback in Bootloader

Use the existing `WORD_LED` (PA0) and `STAT_LED` (PA15) to indicate bootloader state:
- **Slow blink (both LEDs):** Waiting in DFU mode
- **Fast blink (STAT_LED):** Transfer in progress
- **Solid on (WORD_LED):** Transfer complete, preparing to jump

### B5. Bootloader + Application Build and Flash Workflow

1. Build bootloader → flash to `0x08000000`.
2. Build application → flash to `0x08004000` (or use DFU once the bootloader is in place).
3. For subsequent updates, use the DFU path only (no debugger required).

---

## Common Steps (Both Approaches)

### C1. USB VID/PID Considerations

- The ROM bootloader uses ST's VID/PID (`0x0483` / `0xDF11`). No driver installation is needed on Linux/macOS; Windows requires Zadig with the WinUSB driver.
- A custom bootloader can use the same VID/PID or a custom pair if a vendor DFU driver is preferred.

### C2. dfu-util Installation

| Platform | Command |
|----------|---------|
| Windows  | Download from https://dfu-util.sourceforge.net or install via Chocolatey: `choco install dfu-util` |
| macOS    | `brew install dfu-util` |
| Linux    | `sudo apt install dfu-util` |

### C3. Firmware Binary Format

- STM32CubeIDE outputs `.elf`, `.hex`, and `.bin` by default.
- `dfu-util` accepts `.bin` (raw binary) or `.dfu` (DFU suffix format).
- STM32CubeProgrammer accepts `.bin`, `.hex`, and `.elf`.
- For the ROM bootloader (Approach A), use `firmware.bin` targeted at `0x08000000`.
- For the custom bootloader (Approach B), use `application.bin` targeted at `0x08004000`.

### C4. Testing Checklist

- [ ] Device enumerates as USB DFU after trigger
- [ ] `dfu-util -l` lists the device and correct alternate setting
- [ ] Full firmware download completes without errors
- [ ] Device resets and runs the new firmware correctly
- [ ] Magic word is cleared so the device does not re-enter DFU on next power cycle
- [ ] Normal CDC operation is unaffected after a no-DFU boot

---

## Recommended Implementation Order

1. Implement the ROM bootloader jump (Approach A) as the first milestone — this requires no new binary, no flash layout changes, and provides immediate DFU capability.
2. Add a `CMD_ENTER_DFU` command to the existing frame protocol so the host can trigger DFU over the same WebSerial connection.
3. Evaluate whether the ROM bootloader limitations (no firmware validation, no progress feedback) are acceptable for the target use case.
4. If not, proceed with Approach B to build a custom bootloader, using the ROM-DFU path as a fallback recovery mechanism (accessible by asserting BOOT0).

---

## References

- [STM32G4 Reference Manual (RM0440)](https://www.st.com/resource/en/reference_manual/rm0440-stm32g4-series-advanced-armbased-32bit-mcus-stmicroelectronics.pdf) — Section on system memory bootloader and USB
- [AN2606 – STM32 microcontroller system memory boot mode](https://www.st.com/resource/en/application_note/an2606-stm32-microcontroller-system-memory-boot-mode-stmicroelectronics.pdf) — DFU support matrix and USB clock requirements
- [AN3156 – USB DFU protocol used in the STM32 bootloader](https://www.st.com/resource/en/application_note/an3156-usb-dfu-protocol-used-in-the-stm32-bootloader-stmicroelectronics.pdf)
- [UM2237 – STM32CubeProgrammer software description](https://www.st.com/resource/en/user_manual/um2237-stm32cubeprogrammer-software-description-stmicroelectronics.pdf)
- [dfu-util homepage](https://dfu-util.sourceforge.net)
