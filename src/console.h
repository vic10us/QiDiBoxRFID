#pragma once

// Initialize stdio for USB-Serial-JTAG / UART (unbuffered, line-mode)
// and start a background task that reads commands from stdin.
void console_start(void);
