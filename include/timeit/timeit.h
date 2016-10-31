/*
 * timeit.h - Timeit.
 *
 * author       : Jeroen van der Heijden
 * email        : jeroen@transceptor.technology
 * copyright    : 2016, Transceptor Technology
 *
 * changes
 *  - initial version, 16-03-2016
 *
 */
#pragma once

#include <time.h>

double timeit_stop(struct timespec * start);

/*
 * Usage:
 *
 *  struct timespec start;
 *  timeit_start(&start);
 *
 *  ... some code ....
 *
 *  log_debug("Time in seconds: %f",timeit_stop(&start));
 */
#define timeit_start(start) clock_gettime(CLOCK_MONOTONIC, start)
