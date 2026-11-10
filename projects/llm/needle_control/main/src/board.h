#ifndef BOARD_H
#define BOARD_H

#include <stddef.h>

/*
 * The actions the model can call, one per tool in tools.json. Each writes a
 * one-line description of what happened into result and returns 0, or a
 * negative value when the action failed.
 */
int board_take_photo(char *result, size_t size);
int board_beep(int times, char *result, size_t size);
/* Returns at once; the beep comes when the countdown ends. */
int board_set_timer(int seconds, char *result, size_t size);
int board_get_memory(char *result, size_t size);
int board_get_ip_address(char *result, size_t size);

#endif
