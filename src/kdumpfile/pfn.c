/** @internal @file src/kdumpfile/pfn.c
 * @brief Routines for mapping PFN ranges to file offsets.
 */
/* Copyright (C) 2022 Petr Tesarik <ptesarik@suse.com>

   This file is free software; you can redistribute it and/or modify
   it under the terms of either

     * the GNU Lesser General Public License as published by the Free
       Software Foundation; either version 3 of the License, or (at
       your option) any later version

   or

     * the GNU General Public License as published by the Free
       Software Foundation; either version 2 of the License, or (at
       your option) any later version

   or both in parallel, as here.

   libkdumpfile is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received copies of the GNU General Public License and
   the GNU Lesser General Public License along with this program.  If
   not, see <http://www.gnu.org/licenses/>.
*/

#define _GNU_SOURCE

#include "kdumpfile-priv.h"

#include <stdlib.h>

/** Region mapping allocation increment.
 * For optimal performance, this should be a power of two.
 */
#define RGN_ALLOC_INC	1024

/** Add a new PFN region.
 * @param map  Mapping from PFN to file.
 * @param rgn  New PFN region.
 * @returns    Pointer to the new region inside @p regions,
 *             or @c NULL on allocation failure.
 */
struct pfn_region *
add_pfn_region(struct pfn_file_map *map, const struct pfn_region *rgn)
{
	struct pfn_region *ret;

	if (map->nregions % RGN_ALLOC_INC == 0) {
		size_t num = map->nregions + RGN_ALLOC_INC;
		struct pfn_region *new_regions =
			realloc(map->regions, num * sizeof(struct pfn_region));
		if (!new_regions)
			return NULL;
		map->regions = new_regions;
	}

	ret = map->regions + map->nregions;
	*ret = *rgn;
	++map->nregions;
	return ret;
}

/** Find a PFN region by PFN.
 * @param map  Mapping from PFN to file.
 * @param pfn  Page frame number.
 * @returns    Pointer to a PFN region which contains @c pfn or the closest
 *             higher PFN, or @c NULL if there is no such region.
 */
const struct pfn_region *
find_pfn_region(const struct pfn_file_map *map, kdump_pfn_t pfn)
{
	size_t left = 0, right = map->nregions;
	while (left != right) {
		size_t mid = (left + right) / 2;
		const struct pfn_region *rgn = map->regions + mid;
		if (pfn < rgn->pfn)
			right = mid;
		else if (pfn >= rgn->pfn + rgn->cnt)
			left = mid + 1;
		else
			return rgn;
	}
	return right < map->nregions
		? map->regions + right
		: NULL;
}

/** Compare two PFN-to-file maps for @c qsort.
 * @param a  Pointer to first pdmap.
 * @param b  Pointer to second pdmap.
 * @returns  Result of comparison.
 */
static int
map_cmp(const void *a, const void *b)
{
	const struct pfn_file_map *mapa = a, *mapb = b;
	return mapa->end_pfn != mapb->end_pfn
		? (mapa->end_pfn > mapb->end_pfn ? 1 : -1)
		: 0;
}

/** Sort an array of PFN-to-file maps.
 * @param maps   Array of PFN-to-file maps.
 * @param nmaps  Number of elements in @p maps.
 */
void
sort_pfn_file_maps(struct pfn_file_map *maps, size_t nmaps)
{
	qsort(maps, nmaps, sizeof *maps, map_cmp);
}