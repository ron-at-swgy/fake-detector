/*
 * fd_crewrift.h -- optional CrewRift identity mapping for libfakedetector.
 *
 * CrewRift (coworld) identifies players by color; the canonical slot
 * order below matches the game's PlayerColorNames wire order, so a
 * policy can use its decoded color index directly as the fd_player id.
 * Include this header and call fd_crewrift_register once per detector
 * to name ids 0..15 accordingly; the core library itself knows nothing
 * about colors.
 */

#ifndef FD_CREWRIFT_H
#define FD_CREWRIFT_H

#include "fakedetector.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FD_CREWRIFT_N_COLORS 16

static const char *const FD_CREWRIFT_COLORS[FD_CREWRIFT_N_COLORS] = {
	"red",    "blue", "green", "pink",
	"orange", "yellow", "purple", "cyan",
	"lime",   "brown", "beige", "navy",
	"teal",   "rose", "maroon", "gray"
};

/*
 * Register all 16 CrewRift color names as player ids 0..15, in wire
 * order. Call after fd_create (names persist across fd_reset).
 * Returns 0 if every registration succeeded, -1 otherwise.
 */
static inline int
fd_crewrift_register(fd_detector *fd)
{
	int rc = 0;
	for (int i = 0; i < FD_CREWRIFT_N_COLORS; i++) {
		if (fd_set_player_name(fd, i, FD_CREWRIFT_COLORS[i]) != 0)
			rc = -1;
	}
	return rc;
}

#ifdef __cplusplus
}
#endif

#endif /* FD_CREWRIFT_H */
