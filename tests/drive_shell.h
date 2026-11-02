#ifndef DRIVE_SHELL_H
#define DRIVE_SHELL_H

#include <stddef.h>

void capture_stdout(void (*fn)(void), char *buf, size_t len);

void drive_shell_tests(int *passed, int *failed);

#endif
