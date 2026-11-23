/*
 * axis-release-episodes.c - what a name says it holds of a series (see the
 * header)
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "release/axis-release.h"

#include <string.h>

void
axis_release_states(
	const AxisRelease *release,
	AxisReleaseStates *states
){
	memset(states, 0, sizeof(*states));
	states->season_first = -1;
	states->season_last = -1;
	if (release == NULL) {
		states->code = "no_episode";
		return;
	}
	/* A daily show: the day it aired. */
	if (release->air_year > 0) {
		states->kind = AXIS_RELEASE_HOLDS_DATE;
		g_snprintf(states->date, sizeof(states->date), "%04d-%02d-%02d", release->air_year, release->air_month,
			release->air_day);
		return;
	}
	if (release->season_first < 0) {
		if (release->complete) {
			states->kind = AXIS_RELEASE_HOLDS_COMPLETE;
			return;
		}
		if (release->absolute_count > 0) {
			states->code = "absolute_only";
			states->absolute = release->absolute[0];
			return;
		}
		states->code = "no_episode";
		return;
	}
	if (release->episode_count == 0) {
		/* "S02 - 05" (anime) states a season and an absolute number: neither
		 * a pack nor an episode it can name. */
		if (release->absolute_count > 0) {
			states->code = "absolute_only";
			states->absolute = release->absolute[0];
			states->with_season = TRUE;
			return;
		}
		states->kind = AXIS_RELEASE_HOLDS_SEASONS;
		states->season_first = release->season_first;
		states->season_last = MAX(release->season_last, release->season_first);
		return;
	}
	if (release->season_last != release->season_first) {
		states->code = "cross_season";
		return;
	}
	states->kind = AXIS_RELEASE_HOLDS_EPISODES;
	states->season_first = release->season_first;
	states->season_last = release->season_first;
	states->n_episodes = MIN(release->episode_count, (guint)AXIS_RELEASE_MAX_NUMBERS);
	states->episodes = release->episodes;
}
