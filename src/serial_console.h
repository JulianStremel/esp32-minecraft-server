// Server console on the serial port: lines typed into the monitor run as commands
// (as on the PC server's stdin), e.g. "perfbar on" or "/save-all".
#pragma once
#include <cstddef>

// Starts the reader task (USB Serial/JTAG and the console UART). Each finished line
// wakes the game loop (plat::wake()).
void serialConsoleStart();
// The next typed line, if any. Call from the game loop only.
bool serialConsoleLine(char* buf, size_t cap);
