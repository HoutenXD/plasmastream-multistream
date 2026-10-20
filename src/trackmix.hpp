/*
PlasmaStream Multistream
Copyright (C) 2026 PlasmaStream

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#pragma once

#include <cstdint>
#include <vector>

#include <obs.h>

namespace plasmastream {

/* Several of OBS's audio tracks added together, for a destination that hears
 * more than one.
 *
 * OBS mixes each of its six tracks on its own, and an audio encoder reads
 * exactly one of them. A destination set to tracks 1, 2 and 4 needs those three
 * as one sound, so this listens to each track's finished mix, adds them up in
 * step, and plays the result out of an audio output of its own that the
 * destination's encoder reads instead of OBS's.
 *
 * In step means by OBS's own timestamps: every track's mix for the same moment
 * carries the same timestamp, and the sum is only made once all of them have
 * arrived, then stamped with that same time. So the sound lines up with the
 * picture exactly as the main stream's does.
 *
 * One per combination of tracks, shared by every destination that chose the
 * same one. */

/* The tracks as a bitmask, bit 0 for track 1. Out-of-range entries are ignored. */
uint32_t tracks_mask(const std::vector<int> &tracks);

/* How many tracks are in a mask. */
int tracks_in(uint32_t mask);

/* An audio output whose mix 0 is the sum of the tracks in the mask, or null if
 * one could not be made. Counted: every acquire needs a release. */
audio_t *track_mix_acquire(uint32_t mask);

/* Done with it. The mixer stops listening at once and is closed as soon as no
 * encoder is attached to it any more. */
void track_mix_release(uint32_t mask);

/* Close every mixer. Module unload only, after every output has been released:
 * the mixers run a thread inside this module, which must not outlive it. */
void track_mix_shutdown();

} // namespace plasmastream
