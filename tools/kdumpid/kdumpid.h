#ifndef __KDUMPID_H
#define __KDUMPID_H

#include "config.h"

#include <stdint.h>
#include <unistd.h>
#include <endian.h>
#include <libkdumpfile/kdumpfile.h>

/* Older glibc didn't have the byteorder macros */
#ifndef be16toh

#include <byteswap.h>

# if __BYTE_ORDER == __LITTLE_ENDIAN
#  define htobe16(x) bswap_16(x)
#  define htole16(x) (x)
#  define be16toh(x) bswap_16(x)
#  define le16toh(x) (x)

#  define htobe32(x) bswap_32(x)
#  define htole32(x) (x)
#  define be32toh(x) bswap_32(x)
#  define le32toh(x) (x)

#  define htobe64(x) bswap_64(x)
#  define htole64(x) (x)
#  define be64toh(x) bswap_64(x)
#  define le64toh(x) (x)
# else
#  define htobe16(x) (x)
#  define htole16(x) bswap_16(x)
#  define be16toh(x) (x)
#  define le16toh(x) bswap_16(x)

#  define htobe32(x) (x)
#  define htole32(x) bswap_32(x)
#  define be32toh(x) (x)
#  define le32toh(x) bswap_32(x)

#  define htobe64(x) (x)
#  define htole64(x) bswap_64(x)
#  define be64toh(x) (x)
#  define le64toh(x) bswap_64(x)
# endif

#endif

#define INVALID_ADDR	((uint64_t)-1ULL)

struct dump_desc;

struct dump_desc {
	const char *name;	/* file name */
	long flags;		/* see DIF_XXX below */
	int fd;			/* dump file descriptor */
	kdump_ctx_t *ctx;	/* kdumpfile context */

	void *page;		/* page data buffer */
	kdump_num_t page_size;	/* target page size */
	kdump_num_t max_pfn;	/* max PFN for read_page */

	const char *format;	/* format name */

	const char *arch;	/* architecture (if known) */
	kdump_num_t endian;	/* target byte order */
	uint64_t start_addr;	/* kernel start address */

	char machine[66];	/* arch name (utsname machine) */
	char ver[66];		/* version (utsname release) */
	char banner[256];	/* Linux banner */

	char *cfg;		/* kernel configuration */
	size_t cfglen;

	kdump_num_t xen_type;	 /* Xen dump type (or kdump_xen_none) */
	uint64_t xen_start_info; /* address of Xen start info */

	void *priv;
};

/* Kdumpid flags */
#define DIF_VERBOSE	1
#define DIF_FORCE	2
#define DIF_START_FOUND	8

/* Arch-specific helpers */
int looks_like_kcode_ppc(struct dump_desc *dd, uint64_t addr);
int looks_like_kcode_ppc64(struct dump_desc *dd, uint64_t addr);
int looks_like_kcode_s390(struct dump_desc *dd, uint64_t addr);
int looks_like_kcode_x86(struct dump_desc *dd, uint64_t addr);

/* provide our own definition of new_utsname */
struct new_utsname {
	char sysname[65];
	char nodename[65];
	char release[65];
	char version[65];
	char machine[65];
	char domainname[65];
};

/* utils */

int get_version_from_banner(struct dump_desc *dd);
int need_explore(struct dump_desc *dd);

int read_page(struct dump_desc *dd, unsigned long pfn);
size_t dump_cpin(struct dump_desc *dd, void *buf, uint64_t paddr, size_t len);

int uncompress_config(struct dump_desc *dd, void *zcfg, size_t zsize);
uint64_t dump_search_range(struct dump_desc *dd,
			   uint64_t start, uint64_t end,
			   const unsigned char *needle, size_t len);

int explore_raw_data(struct dump_desc *dd);

#endif	/* __KDUMPID_H */
