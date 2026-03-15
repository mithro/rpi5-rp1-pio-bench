/* pio_unclaim.c — Release all PIO state machine claims
 * Usage: sudo ./pio_unclaim
 */
#include <stdio.h>
#include "piolib.h"

int main(void)
{
	PIO pio = pio_open(0);
	if (!pio) {
		fprintf(stderr, "pio_open failed\n");
		return 1;
	}
	for (int sm = 0; sm < 4; sm++) {
		pio_sm_set_enabled(pio, sm, false);
		pio_sm_unclaim(pio, sm);
		printf("SM%d: disabled + unclaimed\n", sm);
	}
	int free_sm = pio_claim_unused_sm(pio, false);
	printf("First free SM after unclaim: %d\n", free_sm);
	if (free_sm >= 0)
		pio_sm_unclaim(pio, free_sm);
	pio_close(pio);
	return 0;
}
