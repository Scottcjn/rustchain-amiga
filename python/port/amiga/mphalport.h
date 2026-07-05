// HAL declarations for the m68k AmigaOS MicroPython port.
static inline void mp_hal_set_interrupt_char(char c) {
    (void)c; // no async keyboard interrupt on the cooked AmigaDOS console
}
