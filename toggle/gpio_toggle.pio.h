/* gpio_toggle.pio.h — Auto-generated from gpio_toggle.pio by pioasm
 *
 * DO NOT EDIT — regenerate with: pioasm gpio_toggle.pio gpio_toggle.pio.h
 */

#ifndef GPIO_TOGGLE_PIO_H
#define GPIO_TOGGLE_PIO_H

#include "pio_platform.h"

static const uint16_t gpio_toggle_program_instructions[] = {
    0xe001, /*  0: set pins, 1                  */
    0xe000, /*  1: set pins, 0                  */
};

static const struct pio_program gpio_toggle_program = {
    .instructions = gpio_toggle_program_instructions,
    .length = 2,
    .origin = -1,
};

static inline pio_sm_config gpio_toggle_program_get_default_config(uint offset)
{
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + 0, offset + 1);
    return c;
}

#endif /* GPIO_TOGGLE_PIO_H */
