#include "pk_game.h"
#include <stdio.h>
#include <string.h>

#define PK_DEBOUNCE_MS         20u
#define PK_READY_TIMEOUT_MS 10000u
#define PK_MIN_WAIT_MS       2000u
#define PK_WAIT_SPAN_MS      3001u
#define PK_RESPONSE_MS       5000u
#define PK_TARGET_SCORE         3u
#define PK_BUZZ_OK_MS         100u
#define PK_BUZZ_OTHER_MS      450u

/* Comparisons of unsigned tick differences remain correct across HAL tick wrap. */
typedef enum { IDLE, ARM, WAIT, GO, RESULT, DONE } Phase;

typedef struct {
    uint8_t raw_pressed, stable_pressed, press_event;
    uint32_t raw_changed_at, pressed_at;
} Button;

static PK_IO out;
static Button buttons[2];
static uint32_t rng_state;
static uint32_t entered_at, wait_deadline, go_at;
static uint32_t early_confirmed_at;
static uint32_t beep_started_at, beep_duration_ms;
static uint32_t match_id, round_id;
static int32_t p1_ms, p2_ms;
static uint8_t s1, s2, winner, early_first, buzzer_on;
static Phase phase;
static const char *reason;

static const char *phase_name(Phase x)
{
    switch (x) {
    case IDLE:   return "IDLE";
    case ARM:    return "ARM";
    case WAIT:   return "WAIT";
    case GO:     return "GO";
    case RESULT: return "RESULT";
    case DONE:   return "DONE";
    default:     return "IDLE";
    }
}

static uint32_t random32(void)
{
    /* A non-cryptographic PRNG, appropriate for an educational reaction game. */
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static void lights(uint8_t red, uint8_t green)
{
    if (out.set_red)   out.set_red(red);
    if (out.set_green) out.set_green(green);
}

static void beep_start(uint32_t now, uint32_t duration)
{
    beep_started_at = now;
    beep_duration_ms = duration;
    buzzer_on = 1u;
    if (out.set_buzzer) out.set_buzzer(1u);
}

static void beep_stop(void)
{
    buzzer_on = 0u;
    if (out.set_buzzer) out.set_buzzer(0u);
}

static void emit(const char *event, const char *message_reason)
{
    char line[260];
    int n = snprintf(line, sizeof(line),
        "{\"v\":1,\"event\":\"%s\",\"match\":%lu,\"round\":%lu,"
        "\"phase\":\"%s\",\"p1_ms\":%ld,\"p2_ms\":%ld,"
        "\"s1\":%u,\"s2\":%u,\"target\":%u,"
        "\"winner\":%u,\"reason\":\"%s\"}",
        event, (unsigned long)match_id, (unsigned long)round_id,
        phase_name(phase), (long)p1_ms, (long)p2_ms,
        (unsigned)s1, (unsigned)s2, (unsigned)PK_TARGET_SCORE,
        (unsigned)winner, message_reason ? message_reason : reason);
    if (n > 0 && (size_t)n < sizeof(line) && out.send_line) {
        out.send_line(line);
    }
}

static void enter_phase(Phase next, uint32_t now, const char *event)
{
    phase = next;
    entered_at = now;
    switch (next) {
    case ARM: case WAIT: lights(1u, 0u); break;
    case GO:             lights(0u, 1u); break;
    default:             lights(0u, 0u); break;
    }
    emit(event, NULL);
}

static void button_sample(Button *b, uint8_t pressed, uint32_t now)
{
    pressed = (pressed != 0u) ? 1u : 0u;
    b->press_event = 0u;
    if (b->raw_pressed != pressed) {
        b->raw_pressed = pressed;
        b->raw_changed_at = now;
    }
    if ((b->raw_pressed != b->stable_pressed) &&
        (uint32_t)(now - b->raw_changed_at) >= PK_DEBOUNCE_MS) {
        b->stable_pressed = b->raw_pressed;
        if (b->stable_pressed) {
            b->press_event = 1u;
            b->pressed_at = b->raw_changed_at; /* compensate for debounce */
        }
    }
}

static uint8_t both_released_for_20ms(uint32_t now)
{
    for (unsigned i = 0u; i < 2u; ++i) {
        if (buttons[i].raw_pressed || buttons[i].stable_pressed ||
            (uint32_t)(now - buttons[i].raw_changed_at) < PK_DEBOUNCE_MS) {
            return 0u;
        }
    }
    return 1u;
}

static void finish(uint32_t now, uint8_t winning_player, const char *why)
{
    winner = winning_player;
    reason = why;
    if (winner == 1u && s1 < PK_TARGET_SCORE) ++s1;
    if (winner == 2u && s2 < PK_TARGET_SCORE) ++s2;
    beep_start(now, (strcmp(why, "OK") == 0) ? PK_BUZZ_OK_MS : PK_BUZZ_OTHER_MS);
    enter_phase((s1 >= PK_TARGET_SCORE || s2 >= PK_TARGET_SCORE) ? DONE : RESULT,
                now, "RESULT");
}

static void finish_early(uint32_t now)
{
    p1_ms = p2_ms = -1;
    if (buttons[0].stable_pressed && buttons[1].stable_pressed) {
        finish(now, 0u, "BOTH_EARLY");
    } else if (early_first == 1u) {
        finish(now, 2u, "P1_EARLY");
    } else {
        finish(now, 1u, "P2_EARLY");
    }
}

static void finish_reaction(uint32_t now)
{
    if (p1_ms >= 0 && p2_ms >= 0) {
        if (p1_ms < p2_ms) finish(now, 1u, "OK");
        else if (p2_ms < p1_ms) finish(now, 2u, "OK");
        else finish(now, 0u, "TIE");
    } else if (p1_ms >= 0) {
        finish(now, 1u, "P2_TIMEOUT");
    } else if (p2_ms >= 0) {
        finish(now, 2u, "P1_TIMEOUT");
    } else {
        finish(now, 0u, "BOTH_TIMEOUT");
    }
}

void PK_Init(const PK_IO *io, uint32_t initial_tick, uint32_t random_seed)
{
    memset(&out, 0, sizeof(out));
    if (io) out = *io;
    memset(buttons, 0, sizeof(buttons));
    buttons[0].raw_changed_at = initial_tick;
    buttons[1].raw_changed_at = initial_tick;
    rng_state = random_seed ? random_seed : 0xA341316Cu;
    match_id = 1u;
    round_id = 0u;
    s1 = s2 = winner = early_first = buzzer_on = 0u;
    p1_ms = p2_ms = -1;
    reason = "NONE";
    phase = IDLE;
    entered_at = initial_tick;
    lights(0u, 0u);
    beep_stop();
}

void PK_Update(uint32_t now, uint8_t p1_pressed, uint8_t p2_pressed)
{
    button_sample(&buttons[0], p1_pressed, now);
    button_sample(&buttons[1], p2_pressed, now);

    if (buzzer_on && (uint32_t)(now - beep_started_at) >= beep_duration_ms) {
        beep_stop();
    }

    if (phase == ARM) {
        if (both_released_for_20ms(now)) {
            early_first = 0u;
            wait_deadline = now + PK_MIN_WAIT_MS + (random32() % PK_WAIT_SPAN_MS);
            enter_phase(WAIT, now, "WAIT");
        } else if ((uint32_t)(now - entered_at) >= PK_READY_TIMEOUT_MS) {
            p1_ms = p2_ms = -1;
            finish(now, 0u, "NOT_READY");
        }
        return;
    }

    if (phase == WAIT) {
        /* First confirmed early button freezes the launch, allowing the other
           player 20 ms after confirmation for BOTH_EARLY. */
        if (!early_first) {
            if (buttons[0].press_event || buttons[1].press_event) {
                early_first = buttons[0].press_event ? 1u : 2u;
                early_confirmed_at = now;
                if (buttons[0].press_event && buttons[1].press_event) {
                    finish(now, 0u, "BOTH_EARLY");
                    return;
                }
            }
        }
        if (early_first) {
            if ((early_first == 1u && buttons[1].press_event) ||
                (early_first == 2u && buttons[0].press_event)) {
                finish(now, 0u, "BOTH_EARLY");
            } else if ((uint32_t)(now - early_confirmed_at) >= PK_DEBOUNCE_MS) {
                finish_early(now);
            }
            return;
        }
        /* Never light the green LED while a low-going transition is pending.
           If held for 20ms it will be counted as a false start. */
        if ((int32_t)(now - wait_deadline) >= 0 &&
            !buttons[0].raw_pressed && !buttons[1].raw_pressed) {
            go_at = now;
            enter_phase(GO, now, "GO");
        }
        return;
    }

    if (phase == GO) {
        for (unsigned i = 0u; i < 2u; ++i) {
            if (buttons[i].press_event) {
                const uint32_t elapsed = buttons[i].pressed_at - go_at;
                int32_t *slot = (i == 0u) ? &p1_ms : &p2_ms;
                if (*slot < 0 && elapsed < PK_RESPONSE_MS) {
                    *slot = (int32_t)elapsed;
                }
            }
        }
        if (p1_ms >= 0 && p2_ms >= 0) {
            finish_reaction(now);
        } else if ((uint32_t)(now - go_at) >= PK_RESPONSE_MS) {
            /* A press that started BEFORE the 5000-ms boundary might still be
               in its 20-ms confirmation interval. Wait for that interval only. */
            uint8_t pending_valid = 0u;
            for (unsigned i = 0u; i < 2u; ++i) {
                if (buttons[i].raw_pressed && !buttons[i].stable_pressed &&
                    (uint32_t)(buttons[i].raw_changed_at - go_at) < PK_RESPONSE_MS &&
                    (uint32_t)(now - buttons[i].raw_changed_at) < PK_DEBOUNCE_MS) {
                    pending_valid = 1u;
                }
            }
            if (!pending_valid) finish_reaction(now);
        }
    }
}

void PK_Command(const char *command, uint32_t now)
{
    if (command == NULL) return;
    if (strcmp(command, "HELLO") == 0 || strcmp(command, "STATUS") == 0) {
        emit(command, NULL);
    } else if (strcmp(command, "START") == 0) {
        if (phase == ARM || phase == WAIT || phase == GO) {
            emit("ERROR", "BUSY");
        } else if (phase == DONE) {
            emit("ERROR", "MATCH_DONE");
        } else {
            ++round_id;
            winner = early_first = 0u;
            p1_ms = p2_ms = -1;
            reason = "NONE";
            /* Fresh round always starts with buzzer off. */
            beep_stop();
            enter_phase(ARM, now, "ARM");
        }
    } else if (strcmp(command, "NEW") == 0) {
        if (phase == ARM || phase == WAIT || phase == GO) {
            emit("ERROR", "BUSY");
        } else {
            ++match_id;
            round_id = 0u;
            s1 = s2 = winner = early_first = 0u;
            p1_ms = p2_ms = -1;
            reason = "NONE";
            beep_stop();
            enter_phase(IDLE, now, "NEW");
        }
    } else if (strcmp(command, "CANCEL") == 0) {
        if (phase == ARM || phase == WAIT || phase == GO) {
            p1_ms = p2_ms = -1;
            finish(now, 0u, "CANCELLED");
        } else {
            emit("ERROR", "NOT_ACTIVE");
        }
    } else {
        emit("ERROR", "BAD_COMMAND");
    }
}
