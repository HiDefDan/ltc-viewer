#ifndef GPIO_H
#define GPIO_H

#include <stdint.h>

/* GPIO edge capture using /dev/mem for DMA timestamps
   Currently a stub. Integration with pigpio will replace this.
   
   Expected interface:
   - gpio_init(): set up BCM2835 GPIO + DMA timer
   - gpio_wait_edge(): block until next GPIO edge, return µs timestamp + level
   - gpio_cleanup(): release resources
*/

typedef struct {
    int gpio_pin;                   /* BCM pin number (e.g., 17) */
    uint64_t last_edge_timestamp;   /* µs */
    int last_edge_level;            /* 0 or 1 */
} gpio_context_t;

/* Initialize GPIO on specified BCM pin for edge capture */
int gpio_init(gpio_context_t *ctx, int bcm_pin);

/* Wait for next GPIO edge (blocking). Returns timestamp (µs) and level (0 or 1). */
int gpio_wait_edge(gpio_context_t *ctx, uint64_t *timestamp_us, int *level);

/* Clean up GPIO */
void gpio_cleanup(gpio_context_t *ctx);

#endif /* GPIO_H */
