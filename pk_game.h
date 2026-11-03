#ifndef PK_GAME_H
#define PK_GAME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pure game logic: no HAL calls. Supply the HAL GPIO/UART adapters in main.c.
   Call PK_Update frequently (preferably many times per 1-ms HAL tick).
   Button arguments: 1 = physically pressed/LOW, 0 = released/HIGH. */
typedef struct {
    void (*send_line)(const char *line); /* complete ASCII JSON without LF */
    void (*set_green)(uint8_t on);
    void (*set_red)(uint8_t on);
    void (*set_buzzer)(uint8_t on);
} PK_IO;

void PK_Init(const PK_IO *io, uint32_t initial_tick, uint32_t random_seed);
void PK_Update(uint32_t now_ms, uint8_t p1_pressed, uint8_t p2_pressed);
void PK_Command(const char *ascii_command, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
#endif
