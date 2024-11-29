/** @internal @file src/addrxlat/x86_64.c
 * @brief Routines specific to AMD64 and Intel 64.
 */
/* Copyright (C) 2016 Petr Tesarik <ptesarik@suse.com>

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

#include <stdlib.h>

#include "addrxlat-priv.h"

/* Maximum physical address bits (architectural limit) */
#define PHYSADDR_BITS_MAX	52
#define PHYSADDR_MASK		ADDR_MASK(PHYSADDR_BITS_MAX)

#define _PAGE_BIT_PRESENT	0
#define _PAGE_BIT_PSE		7

#define _PAGE_PRESENT	(1UL << _PAGE_BIT_PRESENT)
#define _PAGE_PSE	(1UL << _PAGE_BIT_PSE)

/** Maximum virtual address bits (architecture limit). */
#define VIRTADDR_BITS_MAX	48

/** Maximum virtual address bits for 5-level paging (architecture limit). */
#define VIRTADDR_5L_BITS_MAX	57

/** Position of the LA57 bit in CR4. */
#define CR4_BIT_LA57		12

/** Check whether LA57 (5-level paging) is enabled in CR4. */
#define CR4_LA57_ISSET(val)	((val) & ((uint64_t)1 << CR4_BIT_LA57))

/** Page shift (log2 4K). */
#define PAGE_SHIFT		12

/** Page mask. */
#define PAGE_MASK		ADDR_MASK(PAGE_SHIFT)

/** 2M page shift (log2 2M). */
#define PAGE_SHIFT_2M		21

/** 2M page mask. */
#define PAGE_MASK_2M		ADDR_MASK(PAGE_SHIFT_2M)

/** 1G page shift (log2 1G). */
#define PAGE_SHIFT_1G		30

/** 1G page mask. */
#define PAGE_MASK_1G		ADDR_MASK(PAGE_SHIFT_1G)

#define NONCANONICAL_START	((uint64_t)1<<(VIRTADDR_BITS_MAX-1))
#define NONCANONICAL_END	(~NONCANONICAL_START)
#define VIRTADDR_MAX		UINT64_MAX

/* 5-level paging non-canonical extents. */
#define NONCANONICAL_5L_START	((uint64_t)1<<(VIRTADDR_5L_BITS_MAX-1))
#define NONCANONICAL_5L_END	(~NONCANONICAL_5L_START)

/** Virtual address of the Xen machine-to-physical map. */
#define XEN_MACH2PHYS_ADDR	0xffff800000000000

/** Kernel text mapping (virtual address).
 * Note that the start address of this mapping has never changed, so this
 * constant applies to all kernel versions.
 */
#define LINUX_KTEXT_START	0xffffffff80000000

/** Maximum kernel text mapping if kASLR is not active. */
#define LINUX_KTEXT_END_NOKASLR	0xffffffff9fffffff

/** Maximum end of kernel text mapping (virtual address).
 * The kernel text may be smaller, but it must never span beyond this
 * address.
 */
#define LINUX_KTEXT_END		0xffffffffbfffffff

/** Start of direct physical mapping in Linux before 2.6.11 */
#define LINUX_DIRECTMAP_START_2_6_0	0x0000010000000000
/** End of direct physical mapping in Linux before 2.6.11 */
#define LINUX_DIRECTMAP_END_2_6_0	0x000001ffffffffff

/** Start of direct physical mapping in Linux between 2.6.11 and 2.6.27 */
#define LINUX_DIRECTMAP_START_2_6_11	0xffff810000000000
/** End of direct physical mapping in Linux between 2.6.11 and 2.6.27 */
#define LINUX_DIRECTMAP_END_2_6_11	0xffffc0ffffffffff

/** Start of direct physical mapping in Linux between 2.6.27 and 2.6.31 */
#define LINUX_DIRECTMAP_START_2_6_27	0xffff880000000000
/** End of direct physical mapping in Linux between 2.6.27 and 2.6.31 */
#define LINUX_DIRECTMAP_END_2_6_27	0xffffc0ffffffffff

/** Start of direct physical mapping in Linux between 2.6.31 and 4.2 */
#define LINUX_DIRECTMAP_START_2_6_31	LINUX_DIRECTMAP_START_2_6_27
/** End of direct physical mapping in Linux between 2.6.31 and 4.2 */
#define LINUX_DIRECTMAP_END_2_6_31	0xffffc7ffffffffff

/** Start of direct physical mapping in Linux 4.2+ */
#define LINUX_DIRECTMAP_START_4_2	0xffff888000000000
/** End of direct physical mapping in Linux 4.2+ */
#define LINUX_DIRECTMAP_END_4_2		0xffffc8ffffffffff

/** Start of direct physical mapping with 5-level paging before 4.2 */
#define LINUX_DIRECTMAP_START_5L	0xff10000000000000
/** End of direct physical mapping with 5-level paging before 4.2 */
#define LINUX_DIRECTMAP_END_5L		0xff8fffffffffffff

/** Start of direct physical mapping with 5-level paging in 4.2+ */
#define LINUX_DIRECTMAP_START_5L_4_2	0xff11000000000000
/** End of direct physical mapping with 5-level paging in 4.2+ */
#define LINUX_DIRECTMAP_END_5L_4_2	0xff90ffffffffffff

/** Linux Page Table Isolation bit in CR3. */
#define LINUX_PTI_USER_PGTABLE_MASK	((addrxlat_addr_t)1 << PAGE_SHIFT)

/** AMD64 (Intel 64) page table step function.
 * @param step  Current step state.
 * @returns     Error status.
 */
addrxlat_status
pgt_x86_64(addrxlat_step_t *step)
{
	static const char pgt_full_name[][16] = {
		"Page",
		"Page table",
		"Page directory",
		"PDPT table",
		"PML4 table",
	};
	static const char pte_name[][4] = {
		"pte",
		"pmd",
		"pud",
		"p4d",
		"pgd",
	};
	addrxlat_pte_t pte;
	addrxlat_status status;

	status = read_pte64(step, &pte);
	if (status != ADDRXLAT_OK)
		return status;

	if (!(pte & _PAGE_PRESENT))
		return !step->ctx->noerr.notpresent
			? set_error(step->ctx, ADDRXLAT_ERR_NOTPRESENT,
				    "%s not present: %s[%u] = 0x%" ADDRXLAT_PRIxPTE,
				    pgt_full_name[step->remain - 1],
				    pte_name[step->remain - 1],
				    (unsigned) step->idx[step->remain],
				    step->raw.pte)
			: ADDRXLAT_ERR_NOTPRESENT;

	step->base.addr = pte & PHYSADDR_MASK;
	step->base.as = step->meth->target_as;

	if (step->remain == 3 && (pte & _PAGE_PSE)) {
		step->base.addr &= ~PAGE_MASK_1G;
		return pgt_huge_page(step);
	}

	if (step->remain == 2 && (pte & _PAGE_PSE)) {
		step->base.addr &= ~PAGE_MASK_2M;
		return pgt_huge_page(step);
	}

	step->base.addr &= ~PAGE_MASK;
	if (step->remain == 1)
		step->elemsz = 1;

	return ADDRXLAT_OK;
}

/** Translate virtual to kernel physical using page tables.
 * @param sys    Translation system object.
 * @param ctx    Address translation object.
 * @param addr   Address to be translated.
 * @returns      Error status.
 */
static addrxlat_status
vtop_pgt(addrxlat_sys_t *sys, addrxlat_ctx_t *ctx, addrxlat_addr_t *addr)
{
	addrxlat_step_t step;
	addrxlat_status status;

	step.ctx = ctx;
	step.sys = sys;
	step.meth = &sys->meth[ADDRXLAT_SYS_METH_PGT];
	step.base.addr = *addr;
	status = internal_walk(&step);
	if (status != ADDRXLAT_OK)
		return status;

	status = internal_fulladdr_conv(&step.base, ADDRXLAT_KPHYSADDR,
					ctx, sys);
	if (status == ADDRXLAT_OK)
		*addr = step.base.addr;

	return status;
}

/** Remove the method for the reverse direct mapping.
 * @param sys  Translation system.
 */
static void
remove_rdirect(addrxlat_sys_t *sys)
{
	addrxlat_map_t *map;

	sys->meth[ADDRXLAT_SYS_METH_RDIRECT].kind = ADDRXLAT_NOMETH;
	map = sys->map[ADDRXLAT_SYS_MAP_KPHYS_DIRECT];
	if (map) {
		internal_map_decref(map);
		sys->map[ADDRXLAT_SYS_MAP_KPHYS_DIRECT] = NULL;
	}
}

/** Get Linux directmap layout by kernel version.
 * @param rgn  Directmap region; updated on success.
 * @param ver  Version code.
 * @returns    Error status.
 *
 * The @c first and @c last fields of @c rgn are set according to
 * the Linux kernel version if this function returns @ref ADDRXLAT_OK.
 * No error message is set if this function fails, so the caller need
 * not clear it with @ref clear_error.
 */
static addrxlat_status
linux_directmap_by_ver(struct sys_region *rgn, unsigned ver)
{
	if (ver >= ADDRXLAT_VER_LINUX(4, 8, 0))
		return ADDRXLAT_ERR_NOMETH;

#define LINUX_DIRECTMAP_BY_VER(a, b, c)			\
	if (ver >= ADDRXLAT_VER_LINUX(a, b, c)) {		\
		rgn->first = LINUX_DIRECTMAP_START_ ##a##_##b##_##c;	\
		rgn->last = LINUX_DIRECTMAP_END_ ##a##_##b##_##c;	\
		return ADDRXLAT_OK;					\
	}

	LINUX_DIRECTMAP_BY_VER(2, 6, 31);
	LINUX_DIRECTMAP_BY_VER(2, 6, 27);
	LINUX_DIRECTMAP_BY_VER(2, 6, 11);
	LINUX_DIRECTMAP_BY_VER(2, 6, 0);

	return ADDRXLAT_ERR_NOTIMPL;
}

/** Check whether an address looks like start of direct mapping.
 * @param sys    Translation system.
 * @param ctx    Address translation context.
 * @param addr   Address to be checked.
 * @returns      Non-zero if the address maps to physical address 0.
 */
static int
is_directmap(addrxlat_sys_t *sys, addrxlat_ctx_t *ctx,
	     addrxlat_addr_t addr)
{
	addrxlat_status status = vtop_pgt(sys, ctx, &addr);
	clear_error(ctx);
	return status == ADDRXLAT_OK && addr == 0;
}

/** Search for Linux directmap in the page tables.
 * @param rgn   Directmap region; updated on success.
 * @param step  Initial state for page table translation.
 * @returns    Error status.
 */
static addrxlat_status
linux_search_directmap(struct sys_region *rgn, addrxlat_step_t *step)
{
	addrxlat_addr_t end;
	addrxlat_status status;

	if (step->meth->param.pgt.pf.nfields == 6) {
		rgn->first = LINUX_DIRECTMAP_START_5L;
		end = LINUX_DIRECTMAP_END_5L_4_2;
	} else {
		rgn->first = LINUX_DIRECTMAP_START_2_6_31;
		end = LINUX_DIRECTMAP_END_4_2;
	}
	while (rgn->first < end) {
		status = lowest_mapped(step, &rgn->first, end);
		if (status != ADDRXLAT_OK)
			break;
		if (is_directmap(step->sys, step->ctx, rgn->first)) {
			rgn->last = rgn->first;
			return highest_linear(step, &rgn->last, end,
					      -rgn->first);
		}
		status = lowest_unmapped(step, &rgn->first, end);
		if (status != ADDRXLAT_OK)
			break;
	}

	return ADDRXLAT_ERR_NOTIMPL;
}

/** Get directmap location by walking page tables.
 * @param rgn  Directmap region; updated on success.
 * @param sys  Translation system object.
 * @param ctx  Address translation context.
 * @returns    Error status.
 *
 * No error message is set if this function fails, so the caller need
 * not clear it with @ref clear_error.
 */
static addrxlat_status
linux_directmap_by_pgt(struct sys_region *rgn,
		       addrxlat_sys_t *sys, addrxlat_ctx_t *ctx)
{
	addrxlat_step_t step;

	step.ctx = ctx;
	step.sys = sys;
	step.meth = &sys->meth[ADDRXLAT_SYS_METH_PGT];

	if (is_directmap(sys, ctx, LINUX_DIRECTMAP_START_2_6_0)) {
		rgn->last = rgn->first = LINUX_DIRECTMAP_START_2_6_0;
		return highest_linear(&step, &rgn->last,
				      LINUX_DIRECTMAP_END_2_6_0, -rgn->first);
	}

	if (is_directmap(sys, ctx, LINUX_DIRECTMAP_START_2_6_11)) {
		rgn->last = rgn->first = LINUX_DIRECTMAP_START_2_6_11;
		return highest_linear(&step, &rgn->last,
				      LINUX_DIRECTMAP_END_2_6_11, -rgn->first);
	}

	return linux_search_directmap(rgn, &step);
}

/** Set up Linux direct mapping on x86_64.
 * @param ctl  Initialization data.
 * @returns    Error status.
 */
static addrxlat_status
linux_directmap(struct os_init_data *ctl)
{
	struct sys_region layout[2];
	addrxlat_status status;

	if (ctl->sys->meth[ADDRXLAT_SYS_METH_DIRECT].kind != ADDRXLAT_NOMETH)
		return ADDRXLAT_OK;

	status = linux_directmap_by_pgt(&layout[0], ctl->sys, ctl->ctx);
	if (status != ADDRXLAT_OK && opt_isset(ctl->popt, version_code))
		status = linux_directmap_by_ver(&layout[0],
						ctl->popt.version_code);
	remove_rdirect(ctl->sys);
	if (status == ADDRXLAT_OK) {
		layout[0].meth = ADDRXLAT_SYS_METH_DIRECT;
		layout[0].act = SYS_ACT_DIRECT;
		layout[1].meth = ADDRXLAT_SYS_METH_NUM;
		status = sys_set_layout(ctl, ADDRXLAT_SYS_MAP_KV_PHYS, layout);
	}
	return status;
}

/** Set the kernel text mapping offset.
 * @param sys  Translation system object.
 * @param off  Offset from physical to virtual addresses.
 */
static void
set_ktext_offset(addrxlat_sys_t *sys, addrxlat_addr_t off)
{
	addrxlat_meth_t *meth = &sys->meth[ADDRXLAT_SYS_METH_KTEXT];
	meth->kind = ADDRXLAT_LINEAR;
	meth->target_as = ADDRXLAT_KPHYSADDR;
	meth->param.linear.off = off;
}

/** Calculate Linux kernel text mapping offset using page tables.
 * @param sys    Translation system object.
 * @param ctx    Address translation object.
 * @param vaddr  Any valid kernel text virtual address.
 * @returns      Error status.
 */
static addrxlat_status
calc_ktext_offset(addrxlat_sys_t *sys, addrxlat_ctx_t *ctx,
		  addrxlat_addr_t vaddr)
{
	addrxlat_addr_t paddr;
	addrxlat_status status;

	paddr = vaddr;
	status = vtop_pgt(sys, ctx, &paddr);
	if (status != ADDRXLAT_OK)
		return status;

	set_ktext_offset(sys, paddr - vaddr);
	return ADDRXLAT_OK;
}

/** Fall back to page table mapping if needed.
 * @param sys    Translation system object.
 * @param idx    Translation method index.
 *
 * If the corresponding translation method is undefined, fall back
 * to hardware page table mapping.
 */
static void
set_pgt_fallback(addrxlat_sys_t *sys, addrxlat_sys_meth_t idx)
{
	addrxlat_meth_t *meth = &sys->meth[idx];
	if (meth->kind == ADDRXLAT_NOMETH)
		*meth = sys->meth[ADDRXLAT_SYS_METH_PGT];
}

/** Set up Linux kernel reverse direct mapping on x86_64.
 * @param ctl  Initialization data.
 * @returns    Error status.
 */
static addrxlat_status
linux_rdirect_map(struct os_init_data *ctl)
{
	/** Possible direct mapping locations (if not randomized).
	 * Try more recent kernels first.
	 */
	static const addrxlat_addr_t fixed_loc[] = {
		LINUX_DIRECTMAP_START_4_2,
		LINUX_DIRECTMAP_START_2_6_31,
		LINUX_DIRECTMAP_START_2_6_11,
		LINUX_DIRECTMAP_START_2_6_0,
	};

	struct sys_region layout[2];
	int i;
	addrxlat_fulladdr_t page_offset;
	unsigned long read_caps;
	addrxlat_status status;

	read_caps = ctl->ctx->cb->read_caps(ctl->ctx->cb);
	if (!(read_caps & ADDRXLAT_CAPS(ADDRXLAT_KVADDR)))
		return ADDRXLAT_ERR_NOMETH;

	layout[0].first = 0;
	layout[0].last = PHYSADDR_MASK;
	layout[0].meth = ADDRXLAT_SYS_METH_RDIRECT;
	layout[0].act = SYS_ACT_RDIRECT;
	layout[1].meth = ADDRXLAT_SYS_METH_NUM;

	status = get_symval(ctl->ctx, "page_offset_base",
			    &page_offset.addr);
	if (status == ADDRXLAT_OK) {
		uint64_t val;

		page_offset.as = ADDRXLAT_KVADDR;
		status = do_read64(ctl->ctx, &page_offset, &val);
		if (status != ADDRXLAT_OK)
			return status;

		ctl->sys->meth[ADDRXLAT_SYS_METH_DIRECT]
			.param.linear.off = -val;
		status = sys_set_layout(
			ctl, ADDRXLAT_SYS_MAP_KPHYS_DIRECT, layout);
		if (status != ADDRXLAT_OK)
			goto err_layout;

		if (is_directmap(ctl->sys, ctl->ctx, val))
			return ADDRXLAT_OK;
	}

	for (i = 0; i < ARRAY_SIZE(fixed_loc); ++i) {
		ctl->sys->meth[ADDRXLAT_SYS_METH_DIRECT]
			.param.linear.off = -fixed_loc[i];
		status = sys_set_layout(
			ctl, ADDRXLAT_SYS_MAP_KPHYS_DIRECT, layout);
		if (status != ADDRXLAT_OK)
			goto err_layout;

		if (is_directmap(ctl->sys, ctl->ctx, fixed_loc[i]))
			return ADDRXLAT_OK;

		remove_rdirect(ctl->sys);
	}

	return ADDRXLAT_NOMETH;

 err_layout:
	return set_error(ctl->ctx, status, "Cannot set up %s",
			 "Linux kernel direct mapping");
}

/** Set up Linux kernel text translation method.
 * @param ctl  Initialization data.
 * @returns    Error status.
 */
static addrxlat_status
linux_ktext_meth(struct os_init_data *ctl)
{
	addrxlat_addr_t stext;
	addrxlat_status status;

	if (opt_isset(ctl->popt, phys_base)) {
		set_ktext_offset(ctl->sys, (ctl->popt.phys_base -
					    LINUX_KTEXT_START));
		return ADDRXLAT_OK;
	}

	status = get_symval(ctl->ctx, "_stext", &stext);
	if (status == ADDRXLAT_ERR_NODATA) {
		clear_error(ctl->ctx);
		status = get_symval(ctl->ctx, "_text", &stext);
	}
	if (status == ADDRXLAT_ERR_NODATA) {
		addrxlat_step_t step;

		clear_error(ctl->ctx);
		step.ctx = ctl->ctx;
		step.sys = ctl->sys;
		step.meth = &ctl->sys->meth[ADDRXLAT_SYS_METH_PGT];
		stext = LINUX_KTEXT_START;
		status = lowest_mapped(&step, &stext, LINUX_KTEXT_END);
		if (status != ADDRXLAT_OK)
			return status;

		status = internal_fulladdr_conv(&step.base, ADDRXLAT_KPHYSADDR,
						step.ctx, step.sys);
		if (status != ADDRXLAT_OK)
			return status;

		set_ktext_offset(ctl->sys, step.base.addr - stext);
		return ADDRXLAT_OK;
	} else if (status == ADDRXLAT_OK)
		status = calc_ktext_offset(ctl->sys, ctl->ctx, stext);

	if (status != ADDRXLAT_OK)
		return set_error(ctl->ctx, status, "Cannot translate ktext");
	return status;
}

/** Find the kernel text mapping extents.
 * @param ctl   Initialization data.
 * @param low   Lowest ktext address (set on successful return).
 * @param high  Highest ktext address (set on successful return).
 * @returns     Error status.
 */
static addrxlat_status
linux_ktext_extents(struct os_init_data *ctl,
		    addrxlat_addr_t *low, addrxlat_addr_t *high)
{
	addrxlat_addr_t linearoff;
	addrxlat_step_t step;
	addrxlat_status status;

	step.ctx = ctl->ctx;
	step.sys = ctl->sys;
	step.meth = &ctl->sys->meth[ADDRXLAT_SYS_METH_PGT];
	*low = LINUX_KTEXT_START;
	status = lowest_mapped(&step, low, LINUX_KTEXT_END);
	if (status != ADDRXLAT_OK)
		return status;

	linearoff = ctl->sys->meth[ADDRXLAT_SYS_METH_KTEXT].param.linear.off;
	*high = *low;
	if (*high <= LINUX_KTEXT_END_NOKASLR)
		status = highest_linear(&step, high, LINUX_KTEXT_END_NOKASLR,
					linearoff);
	if (status == ADDRXLAT_OK && *high >= LINUX_KTEXT_END_NOKASLR) {
		++*high;
		status = highest_linear(&step, high, LINUX_KTEXT_END,
					linearoff);
		if (status == ADDRXLAT_ERR_NOTPRESENT) {
			clear_error(step.ctx);
			--*high;
			status = ADDRXLAT_OK;
		}
	}
	return status;
}

/** Set up Linux kernel text mapping on x86_64.
 * @param ctl  Initialization data.
 * @returns    Error status.
 */
static addrxlat_status
linux_ktext_map(struct os_init_data *ctl)
{
	addrxlat_range_t range;
	addrxlat_meth_t *meth;
	addrxlat_addr_t low, high;
	addrxlat_status status;

	status = linux_ktext_meth(ctl);
	if (status == ADDRXLAT_ERR_NOMETH ||
	    status == ADDRXLAT_ERR_NODATA ||
	    status == ADDRXLAT_ERR_NOTPRESENT)
		goto err_nonfatal;
	else if (status != ADDRXLAT_OK)
		return set_error(ctl->ctx, status, "Cannot set up %s",
				 "Linux kernel text mapping");

	meth = &ctl->sys->meth[ADDRXLAT_SYS_METH_PGT];
	if (meth->kind == ADDRXLAT_PGT &&
	    meth->param.pgt.root.as == ADDRXLAT_KVADDR) {
		/* minimal ktext mapping for the root page table */
		range.endoff = PAGE_MASK;
		range.meth = ADDRXLAT_SYS_METH_KTEXT;
		status = internal_map_set(
			ctl->sys->map[ADDRXLAT_SYS_MAP_KV_PHYS],
			meth->param.pgt.root.addr, &range);
		if (status != ADDRXLAT_OK)
			return set_error(ctl->ctx, status, "Cannot set up %s",
					 "minimal Linux kernel text mapping");
	}

	status = linux_ktext_extents(ctl, &low, &high);
	if (status == ADDRXLAT_ERR_NOMETH ||
	    status == ADDRXLAT_ERR_NODATA ||
	    status == ADDRXLAT_ERR_NOTPRESENT)
		goto err_nonfatal;
	else if (status != ADDRXLAT_OK)
		return set_error(ctl->ctx, status,
				 "Linux kernel text search failed");

	range.meth = ADDRXLAT_SYS_METH_KTEXT;
	range.endoff = high - low;
	status = internal_map_set(
		ctl->sys->map[ADDRXLAT_SYS_MAP_KV_PHYS], low, &range);
	if (status != ADDRXLAT_OK)
		return set_error(ctl->ctx, status, "Cannot set up %s",
				 "Linux kernel text mapping");

	return ADDRXLAT_OK;

 err_nonfatal:
	clear_error(ctl->ctx);
	return ADDRXLAT_OK;
}

/** Initialize a translation map for Linux on x86_64.
 * @param ctl  Initialization data.
 * @param m2p  Virtual address of the machine-to-physical array.
 */
static void
set_xen_mach2phys(struct os_init_data *ctl, addrxlat_addr_t m2p)
{
	addrxlat_meth_t *meth =
		&ctl->sys->meth[ADDRXLAT_SYS_METH_MACHPHYS_KPHYS];

	meth->kind = ADDRXLAT_MEMARR;
	meth->target_as = ADDRXLAT_KPHYSADDR;
	meth->param.memarr.base.as = ADDRXLAT_KVADDR;
	meth->param.memarr.base.addr = m2p;
	meth->param.memarr.shift = PAGE_SHIFT;
	meth->param.memarr.elemsz = sizeof(uint64_t);
	meth->param.memarr.valsz = sizeof(uint64_t);
}

/** Initialize Xen p2m translation.
 * @param ctl  Initialization data.
 * @returns    Error status.
 */
static addrxlat_status
set_xen_p2m(struct os_init_data *ctl)
{
	static const addrxlat_paging_form_t xen_p2m_pf = {
		.pte_format = ADDRXLAT_PTE_PFN64,
		.nfields = 4,
		.fieldsz = { 12, 9, 9, 9 }
	};

	addrxlat_addr_t p2m_maddr;
	addrxlat_map_t *map;
	addrxlat_meth_t *meth;
	addrxlat_range_t range;
	addrxlat_status status;

	map = ctl->sys->map[ADDRXLAT_SYS_MAP_KPHYS_MACHPHYS];
	map_clear(map);
	if (!opt_isset(ctl->popt, xen_p2m_mfn))
		return ADDRXLAT_OK; /* leave undefined */
	p2m_maddr = ctl->popt.xen_p2m_mfn << PAGE_SHIFT;

	meth = &ctl->sys->meth[ADDRXLAT_SYS_METH_KPHYS_MACHPHYS];
	meth->kind = ADDRXLAT_PGT;
	meth->target_as = ADDRXLAT_MACHPHYSADDR;
	meth->param.pgt.root.addr = p2m_maddr;
	meth->param.pgt.root.as = ADDRXLAT_MACHPHYSADDR;
	meth->param.pgt.pte_mask = 0;
	meth->param.pgt.pf = xen_p2m_pf;

	range.endoff = paging_max_index(&xen_p2m_pf);
	range.meth = ADDRXLAT_SYS_METH_KPHYS_MACHPHYS;
	status = internal_map_set(map, 0, &range);
	if (status != ADDRXLAT_OK)
		return set_error(ctl->ctx, status,
				 "Cannot allocate Xen p2m map");

	return ADDRXLAT_OK;
}

/** Get the top-level page table address for a Linux kernel.
 * @param ctl   Initialization data.
 * @returns     Error status.
 *
 * It is not an error if the root page table address cannot be
 * determined; it merely stays uninitialized.
 */
static addrxlat_status
get_linux_pgt_root(struct os_init_data *ctl)
{
	static const char err_fmt[] = "Cannot resolve \"%s\"";
	addrxlat_fulladdr_t *addr;
	addrxlat_status status;

	addr = &ctl->sys->meth[ADDRXLAT_SYS_METH_PGT].param.pgt.root;
	if (addr->as != ADDRXLAT_NOADDR)
		return ADDRXLAT_OK;

	status = get_symval(ctl->ctx, "init_top_pgt", &addr->addr);
	if (status == ADDRXLAT_OK) {
		addr->as = ADDRXLAT_KVADDR;
		return status;
	} else if (status != ADDRXLAT_ERR_NODATA)
		return set_error(ctl->ctx, status, err_fmt, "init_top_pgt");
	clear_error(ctl->ctx);

	status = get_symval(ctl->ctx, "init_level4_pgt", &addr->addr);
	if (status == ADDRXLAT_OK) {
		addr->as = ADDRXLAT_KVADDR;
		return status;
	} else if (status != ADDRXLAT_ERR_NODATA)
		return set_error(ctl->ctx, status, err_fmt, "init_level4_pgt");
	clear_error(ctl->ctx);

	status = get_reg(ctl->ctx, "cr3", &addr->addr);
	if (status == ADDRXLAT_OK) {
		addr->addr &= ~PAGE_MASK;
		addr->as = ADDRXLAT_MACHPHYSADDR;
		if (!(addr->addr & LINUX_PTI_USER_PGTABLE_MASK))
			return status;
		status = linux_directmap(ctl);
		if (status == ADDRXLAT_ERR_NOTIMPL) {
			addr->addr &= ~LINUX_PTI_USER_PGTABLE_MASK;
			status = linux_directmap(ctl);
			if (status == ADDRXLAT_OK)
				return status;
			addr->addr |= LINUX_PTI_USER_PGTABLE_MASK;
		}
	} else if (status != ADDRXLAT_ERR_NODATA)
		return set_error(ctl->ctx, status, err_fmt, "cr3");
	clear_error(ctl->ctx);

	return ADDRXLAT_OK;
}

/** Initialize a translation map for Linux on x86_64.
 * @param ctl  Initialization data.
 * @returns    Error status.
 */
static addrxlat_status
map_linux_x86_64(struct os_init_data *ctl)
{
	addrxlat_addr_t sme_mask;
	unsigned long read_caps;
	addrxlat_status status;

	/* Set up page table translation. */
	status = get_linux_pgt_root(ctl);
	if (status != ADDRXLAT_OK)
		return set_error(ctl->ctx, status,
				 "Cannot determine root page table");

	status = get_number(ctl->ctx, "sme_mask", &sme_mask);
	if (status == ADDRXLAT_OK)
		ctl->sys->meth[ADDRXLAT_SYS_METH_PGT].param.pgt.pte_mask =
			sme_mask;
	else if (status == ADDRXLAT_ERR_NODATA)
		clear_error(ctl->ctx);
	else
		return status;

	/* Take care of machine physical <-> kernel physical mapping. */
	if (opt_isset(ctl->popt, xen_xlat) &&
	    ctl->popt.xen_xlat) {
		status = set_xen_p2m(ctl);
		if (status != ADDRXLAT_OK)
			return status;

		set_xen_mach2phys(ctl, XEN_MACH2PHYS_ADDR);
	}

	/* Make sure physical addresses can be accessed.
	 * This is crucial for page table translation.
	 */
	read_caps = ctl->ctx->cb->read_caps(ctl->ctx->cb);
	if (!(read_caps & ADDRXLAT_CAPS(ADDRXLAT_MACHPHYSADDR)) &&
	    !(read_caps & ADDRXLAT_CAPS(ADDRXLAT_KPHYSADDR))) {
		status = linux_rdirect_map(ctl);
		if (status != ADDRXLAT_OK &&
		    status != ADDRXLAT_ERR_NOMETH &&
		    status != ADDRXLAT_ERR_NODATA &&
		    status != ADDRXLAT_ERR_NOTPRESENT)
			return status;

		clear_error(ctl->ctx);
	}

	/* Set up kernel text mapping. */
	status = linux_ktext_map(ctl);
	if (status != ADDRXLAT_OK)
		return status;
	set_pgt_fallback(ctl->sys, ADDRXLAT_SYS_METH_KTEXT);

	/* Set up direct mapping. */
	status = linux_directmap(ctl);
	if (status != ADDRXLAT_OK && status != ADDRXLAT_ERR_NOTIMPL)
		return status;

	return ADDRXLAT_OK;
}

/** Xen direct mapping virtual address. */
#define XEN_DIRECTMAP	0xffff830000000000

/** Xen direct mapping virtual address with Xen 4.6+ BIGMEM. */
#define XEN_DIRECTMAP_BIGMEM	0xffff848000000000

/** Xen 1TB directmap size. */
#define XEN_DIRECTMAP_SIZE_1T	(1ULL << 40)

/** Xen 3.5TB directmap size (BIGMEM). */
#define XEN_DIRECTMAP_SIZE_3_5T	(3584ULL << 30)

/** Xen 5TB directmap size. */
#define XEN_DIRECTMAP_SIZE_5T	(5ULL << 40)

/** Xen 3.2-4.0 text virtual address. */
#define XEN_TEXT_3_2	0xffff828c80000000

/** Xen text virtual address (only during 4.0 development). */
#define XEN_TEXT_4_0dev	0xffff828880000000

/** Xen 4.0-4.3 text virtual address. */
#define XEN_TEXT_4_0	0xffff82c480000000

/** Xen 4.3-4.4 text virtual address. */
#define XEN_TEXT_4_3	0xffff82c4c0000000

/** Xen 4.4+ text virtual address. */
#define XEN_TEXT_4_4	0xffff82d080000000

/** Xen text mapping size. Always 1GB. */
#define XEN_TEXT_SIZE	(1ULL << 30)

/** Check whether an address looks like Xen text mapping.
 * @param ctl    Initialization data.
 * @param addr   Address to be checked.
 * @returns      Non-zero if the address maps to a 2M page.
 */
static int
is_xen_ktext(struct os_init_data *ctl, addrxlat_addr_t addr)
{
	addrxlat_step_t step;
	addrxlat_status status;
	unsigned steps = 0;

	step.ctx = ctl->ctx;
	step.sys = ctl->sys;
	step.meth = &ctl->sys->meth[ADDRXLAT_SYS_METH_PGT];
	status = internal_launch(&step, addr);
	while (status == ADDRXLAT_OK && step.remain) {
		++steps;
		status = internal_step(&step);
	}

	clear_error(ctl->ctx);

	return status == ADDRXLAT_OK && steps == 4;
}

/** Get the top-level page table address for a Xen hypervisor.
 * @param ctx   Address translation object.
 * @param addr  Root page table address. Updated on success.
 * @returns     Error status.
 *
 * It is not an error if the root page table address cannot be
 * determined; it merely stays uninitialized.
 */
static addrxlat_status
get_xen_pgt_root(addrxlat_ctx_t *ctx, addrxlat_fulladdr_t *addr)
{
	static const char err_fmt[] = "Cannot resolve \"%s\"";
	addrxlat_status status;

	if (addr->as != ADDRXLAT_NOADDR)
		return ADDRXLAT_OK;

	status = get_reg(ctx, "cr3", &addr->addr);
	if (status == ADDRXLAT_OK) {
		addr->as = ADDRXLAT_MACHPHYSADDR;
		return status;
	} else if (status != ADDRXLAT_ERR_NODATA)
		return set_error(ctx, status, err_fmt, "cr3");
	clear_error(ctx);

	status = get_symval(ctx, "pgd_l4", &addr->addr);
	if (status == ADDRXLAT_OK) {
		addr->as = ADDRXLAT_KVADDR;
		return status;
	} else if (status != ADDRXLAT_ERR_NODATA)
		return set_error(ctx, status, err_fmt, "pgd_l4");
	clear_error(ctx);

	return ADDRXLAT_OK;
}

/** Initialize temporary mapping to make the page table usable.
 * @param ctl  Initialization data.
 * @returns    Error status.
 */
static addrxlat_status
setup_xen_pgt(struct os_init_data *ctl)
{
	struct sys_region layout[2];
	addrxlat_meth_t *meth;
	addrxlat_addr_t pgt;
	addrxlat_off_t off;
	addrxlat_status status;

	meth = &ctl->sys->meth[ADDRXLAT_SYS_METH_PGT];
	status = get_xen_pgt_root(ctl->ctx, &meth->param.pgt.root);
	meth->param.pgt.root.addr &= ~PAGE_MASK;
	if (meth->param.pgt.root.as != ADDRXLAT_KVADDR)
		return status;	/* either unset or physical */

	pgt = meth->param.pgt.root.addr;
	if (pgt >= XEN_DIRECTMAP) {
		off = -XEN_DIRECTMAP;
	} else if (opt_isset(ctl->popt, phys_base)) {
		addrxlat_addr_t xen_virt_start = pgt & ~(XEN_TEXT_SIZE - 1);
		off = ctl->popt.phys_base - xen_virt_start;
	} else
		return ADDRXLAT_ERR_NODATA;

	/* Temporary linear mapping just for the page table */
	layout[0].first = pgt;
	layout[0].last = pgt + (1ULL << PAGE_SHIFT) - 1;
	layout[0].meth = ADDRXLAT_SYS_METH_KTEXT;
	layout[0].act = SYS_ACT_NONE;

	layout[1].meth = ADDRXLAT_SYS_METH_NUM;

	status = sys_set_layout(ctl, ADDRXLAT_SYS_MAP_KV_PHYS, layout);
	if (status != ADDRXLAT_OK)
		return status;

	set_ktext_offset(ctl->sys, off);
	return ADDRXLAT_OK;
}

/** Initialize a translation map for Xen on x86_64.
 * @param ctl  Initialization data.
 * @returns    Error status.
 */
static addrxlat_status
map_xen_x86_64(struct os_init_data *ctl)
{
	struct sys_region layout[4];
	addrxlat_status status;

	layout[0].first = XEN_DIRECTMAP;
	layout[0].last = XEN_DIRECTMAP + XEN_DIRECTMAP_SIZE_5T - 1;
	layout[0].meth = ADDRXLAT_SYS_METH_DIRECT;
	layout[0].act = SYS_ACT_DIRECT;

	layout[1].meth = ADDRXLAT_SYS_METH_KTEXT;
	layout[1].act = SYS_ACT_NONE;

	layout[2].meth = ADDRXLAT_SYS_METH_NUM;

	setup_xen_pgt(ctl);

	if (is_directmap(ctl->sys, ctl->ctx, XEN_DIRECTMAP)) {
		if (is_xen_ktext(ctl, XEN_TEXT_4_4))
			layout[1].first = XEN_TEXT_4_4;
		else if (is_xen_ktext(ctl, XEN_TEXT_4_3))
			layout[1].first = XEN_TEXT_4_3;
		else if (is_xen_ktext(ctl, XEN_TEXT_4_0))
			layout[1].first = XEN_TEXT_4_0;
		else if (is_xen_ktext(ctl, XEN_TEXT_3_2)) {
			layout[0].last =
				XEN_DIRECTMAP + XEN_DIRECTMAP_SIZE_1T - 1;
			layout[1].first = XEN_TEXT_3_2;
		} else if (is_xen_ktext(ctl, XEN_TEXT_4_0dev))
			layout[1].first = XEN_TEXT_4_0dev;
		else {
			layout[0].last =
				XEN_DIRECTMAP + XEN_DIRECTMAP_SIZE_1T - 1;
			layout[1].meth = ADDRXLAT_SYS_METH_NUM;
		}
	} else if (is_directmap(ctl->sys, ctl->ctx, XEN_DIRECTMAP_BIGMEM)) {
		layout[0].first = XEN_DIRECTMAP_BIGMEM;
		layout[0].last =
			XEN_DIRECTMAP_BIGMEM + XEN_DIRECTMAP_SIZE_3_5T - 1;
		layout[1].first = XEN_TEXT_4_4;
	} else if (opt_isset(ctl->popt, version_code) &&
		   ctl->popt.version_code >= ADDRXLAT_VER_XEN(4, 0)) {
		/* !BIGMEM is assumed for Xen 4.6+. Can we do better? */

		if (ctl->popt.version_code >= ADDRXLAT_VER_XEN(4, 4))
			layout[1].first = XEN_TEXT_4_4;
		else if (ctl->popt.version_code >= ADDRXLAT_VER_XEN(4, 3))
			layout[1].first = XEN_TEXT_4_3;
		else
			layout[1].first = XEN_TEXT_4_0;
	} else if (opt_isset(ctl->popt, version_code)) {
		layout[0].last =
			XEN_DIRECTMAP + XEN_DIRECTMAP_SIZE_1T - 1;

		if (ctl->popt.version_code >= ADDRXLAT_VER_XEN(3, 2))
			layout[1].first = XEN_TEXT_3_2;
		else
			/* Prior to Xen 3.2, text was in direct mapping. */
			layout[1].meth = ADDRXLAT_SYS_METH_NUM;
	} else
		return ADDRXLAT_OK;

	layout[1].last = layout[1].first + XEN_TEXT_SIZE - 1;

	status = sys_set_layout(ctl, ADDRXLAT_SYS_MAP_KV_PHYS, layout);
	if (status != ADDRXLAT_OK)
		return status;

	if (layout[1].meth == ADDRXLAT_SYS_METH_KTEXT) {
		calc_ktext_offset(ctl->sys, ctl->ctx, layout[1].first);
		clear_error(ctl->ctx);
		set_pgt_fallback(ctl->sys, ADDRXLAT_SYS_METH_KTEXT);
	}

	return ADDRXLAT_OK;
}

/** Generic x86_64 layout */
static const struct sys_region layout_generic[] = {
	{  0,  NONCANONICAL_START - 1,		/* lower half       */
	   ADDRXLAT_SYS_METH_PGT },
	/* NONCANONICAL_START - NONCANONICAL_END   non-canonical    */
	{  NONCANONICAL_END + 1,  VIRTADDR_MAX,	/* higher half      */
	   ADDRXLAT_SYS_METH_PGT },
	SYS_REGION_END
};

/** Generic x86_64 5-level paging layout */
static const struct sys_region layout_5level[] = {
	{  0,  NONCANONICAL_5L_START - 1,		/* lower half       */
	   ADDRXLAT_SYS_METH_PGT },
	/* NONCANONICAL_5L_START - NONCANONICAL_5L_END   non-canonical    */
	{  NONCANONICAL_5L_END + 1,  VIRTADDR_MAX,	/* higher half      */
	   ADDRXLAT_SYS_METH_PGT },
	SYS_REGION_END
};

/** Determine the number of virtual address bits.
 * @param ctl      Initialization data.
 * @returns        Error status.
 *
 * On successful return, the virt_bits option is valid.
 */
static addrxlat_status
get_virt_bits(struct os_init_data *ctl)
{
	addrxlat_addr_t cr4;
	addrxlat_status status;

	if (opt_isset(ctl->popt, virt_bits))
		return ADDRXLAT_OK;

	status = get_reg(ctl->ctx, "cr4", &cr4);
	if (status == ADDRXLAT_OK) {
		ctl->popt.virt_bits = CR4_LA57_ISSET(cr4)
			? VIRTADDR_5L_BITS_MAX
			: VIRTADDR_BITS_MAX;
		return ADDRXLAT_OK;
	} else if (status != ADDRXLAT_ERR_NODATA)
		return status;
	clear_error(ctl->ctx);

	if (ctl->os_type == OS_LINUX) {
		addrxlat_addr_t l5_enabled;
		status = get_number(ctl->ctx, "pgtable_l5_enabled",
				    &l5_enabled);
		if (status == ADDRXLAT_OK) {
			ctl->popt.virt_bits = l5_enabled
				? VIRTADDR_5L_BITS_MAX
				: VIRTADDR_BITS_MAX;
			return ADDRXLAT_OK;
		} else if (status != ADDRXLAT_ERR_NODATA)
			return status;
		clear_error(ctl->ctx);

		status = get_symval(ctl->ctx, "_stext", &l5_enabled);
		if (status == ADDRXLAT_OK) {
			ctl->popt.virt_bits = VIRTADDR_BITS_MAX;
			return ADDRXLAT_OK;
		} else if (status != ADDRXLAT_ERR_NODATA)
			return status;
		clear_error(ctl->ctx);

		if (opt_isset(ctl->popt, version_code) &&
		    ctl->popt.version_code < ADDRXLAT_VER_LINUX(4, 13, 0)) {
			ctl->popt.virt_bits = VIRTADDR_BITS_MAX;
			return ADDRXLAT_OK;
		}
	} else if (ctl->os_type == OS_XEN) {
		/* Update this when/if Xen implements 5-level paging. */
		ctl->popt.virt_bits = VIRTADDR_BITS_MAX;
		return ADDRXLAT_OK;
	} else
		status = ADDRXLAT_ERR_NOTIMPL;

	return set_error(ctl->ctx, status,
			 "Cannot determine 5-level paging");
}

/** Initialize the page table translation method.
 * @param ctl      Initialization data.
 * @returns        Error status.
 */
static addrxlat_status
init_pgt_meth(struct os_init_data *ctl)
{
	static const addrxlat_paging_form_t x86_64_pf = {
		.pte_format = ADDRXLAT_PTE_X86_64,
		.nfields = 5,
		.fieldsz = { 12, 9, 9, 9, 9, 9 }
	};

	addrxlat_meth_t *meth;
	addrxlat_status status;

	meth = &ctl->sys->meth[ADDRXLAT_SYS_METH_PGT];
	meth->kind = ADDRXLAT_PGT;
	meth->target_as = ADDRXLAT_MACHPHYSADDR;
	if (opt_isset(ctl->popt, rootpgt))
		meth->param.pgt.root = ctl->popt.rootpgt;
	else
		meth->param.pgt.root.as = ADDRXLAT_NOADDR;
	meth->param.pgt.pte_mask = 0;
	meth->param.pgt.pf = x86_64_pf;

	status = get_virt_bits(ctl);
	if (status != ADDRXLAT_OK)
		return status;

	if (ctl->popt.virt_bits == VIRTADDR_BITS_MAX)
		meth->param.pgt.pf.nfields = 5;
	else if (ctl->popt.virt_bits == VIRTADDR_5L_BITS_MAX)
		meth->param.pgt.pf.nfields = 6;
	else
		return bad_virt_bits(ctl->ctx, ctl->popt.virt_bits);

	return ADDRXLAT_OK;
}

/** Initialize a translation map for an x86_64 OS.
 * @param ctl  Initialization data.
 * @returns    Error status.
 */
addrxlat_status
sys_x86_64(struct os_init_data *ctl)
{
	addrxlat_map_t *map;
	addrxlat_meth_t *meth;
	addrxlat_status status;

	status = init_pgt_meth(ctl);
	if (status != ADDRXLAT_OK)
		return status;

	meth = &ctl->sys->meth[ADDRXLAT_SYS_METH_PGT];
	status = sys_set_layout(ctl, ADDRXLAT_SYS_MAP_HW,
				(meth->param.pgt.pf.nfields == 6
				 ? layout_5level
				 : layout_generic));
	if (status != ADDRXLAT_OK)
		return status;

	map = internal_map_copy(ctl->sys->map[ADDRXLAT_SYS_MAP_HW]);
	if (!map)
		return set_error(ctl->ctx, ADDRXLAT_ERR_NOMEM,
				 "Cannot duplicate hardware mapping");
	ctl->sys->map[ADDRXLAT_SYS_MAP_KV_PHYS] = map;

	status = sys_set_physmaps(ctl, PHYSADDR_MASK);
	if (status != ADDRXLAT_OK)
		return status;

	if (ctl->os_type == OS_LINUX)
		return map_linux_x86_64(ctl);

	if (ctl->os_type == OS_XEN)
		return map_xen_x86_64(ctl);

	return ADDRXLAT_OK;
}
