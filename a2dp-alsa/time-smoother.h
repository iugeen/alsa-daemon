/***
  This file is part of PulseAudio.

  Copyright 2007 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#define bool int
#define true 1
#define false 0

typedef struct pa_smoother pa_smoother;

pa_smoother* pa_smoother_new(
        uint64_t x_adjust_time,
        uint64_t x_history_time,
        bool monotonic,
        bool smoothing,
        unsigned min_history,
        uint64_t x_offset,
        bool paused);

void pa_smoother_free(pa_smoother* s);

/* Adds a new value to our dataset. x = local/system time, y = remote time */
void pa_smoother_put(pa_smoother *s, uint64_t x, uint64_t y);

/* Returns an interpolated value based on the dataset. x = local/system time, return value = remote time */
uint64_t pa_smoother_get(pa_smoother *s, uint64_t x);

/* Translates a time span from the remote time domain to the local one. x = local/system time when to estimate, y_delay = remote time span */
uint64_t pa_smoother_translate(pa_smoother *s, uint64_t x, uint64_t y_delay);

void pa_smoother_set_time_offset(pa_smoother *s, uint64_t x_offset);

void pa_smoother_pause(pa_smoother *s, uint64_t x);
void pa_smoother_resume(pa_smoother *s, uint64_t x, bool abrupt);

void pa_smoother_reset(pa_smoother *s, uint64_t time_offset, bool paused);

void pa_smoother_fix_now(pa_smoother *s);
