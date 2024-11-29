/** @internal @file src/kdumpfile/attr.c
 * @brief Attribute handling.
 */
/* Copyright (C) 2015 Petr Tesarik <ptesarik@suse.cz>

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
#include <string.h>

/** Generic directory attribute template. */
const struct attr_template dir_template = {
	.type = KDUMP_DIRECTORY,
};

#define KDUMP_nil	KDUMP_NIL
#define KDUMP_directory	KDUMP_DIRECTORY
#define KDUMP_number	KDUMP_NUMBER
#define KDUMP_address	KDUMP_ADDRESS
#define KDUMP_string	KDUMP_STRING
#define KDUMP_bitmap	KDUMP_BITMAP
#define KDUMP_blob	KDUMP_BLOB

static const struct attr_template global_keys[] = {
#define ATTR(dir, key, field, type, ctype, ...)				\
	[GKI_ ## field] = {						\
		key,							\
		{ .parent_key = GKI_dir_ ## dir },			\
		KDUMP_ ## type,						\
		##__VA_ARGS__						\
	},
#include "static-attr.def"
#include "global-attr.def"
#undef ATTR
};

static const size_t static_offsets[] = {
#define ATTR(dir, key, field, type, ctype, ...)				\
	[GKI_ ## field - GKI_static_first] =				\
		offsetof(struct kdump_shared, field),
#include "static-attr.def"
#undef ATTR
};

/**  Get a pointer to the static value with a given index.
 * @param shared  Dump file shared data.
 * @param idx     Static index.
 * @returns       Pointer to the static attribute value.
 */
static inline kdump_attr_value_t *
static_attr_value(struct kdump_shared *shared, enum global_keyidx idx)
{
	return (kdump_attr_value_t *)
		((char*)shared + static_offsets[idx - GKI_static_first]);
}

/**  Get the length of an attribute path
 * @param attr  Attribute data.
 * @returns     Length of the full path string.
 *
 * The returned length does not include the terminating NUL character.
 */
static size_t
attr_pathlen(const struct attr_data *attr)
{
	const struct attr_data *d;
	size_t len = 0;

	for (d = attr; d->parent; d = d->parent) {
		len += strlen(d->template->key);
		if (d != attr)
			++len;	/* for the separating dot ('.') */
	}
	return len;
}

/**  Construct an attribute's key path.
 * @param attr  Attribute data.
 * @param endp  Pointer to the __end__ of the path buffer.
 * @returns     Beginning of the path buffer.
 *
 * The output buffer must be big enough to hold the full path. You can
 * use @c attr_pathlen to calculate the required length.
 * Note that the resulting path is a NUL-terminated string, and the buffer
 * must also contain space for this terminating NUL character.
 */
static char *
make_attr_path(const struct attr_data *attr, char *endp)
{
	const struct attr_data *d;

	*endp = '\0';
	for (d = attr; d->parent; d = d->parent) {
		size_t len = strlen(d->template->key);
		if (d != attr)
			*(--endp) = '.';
		endp -= len;
		memcpy(endp, d->template->key, len);
	}
	return endp;
}

/**  Compare if attribute data correponds to a key relative to base.
 * @param attr  Attribute data.
 * @param dir   Base directory attribute.
 * @param key   Key path.
 * @param len   Initial portion of @c key to be considered.
 * @returns     Zero if the data is stored under the given key,
 *              non-zero otherwise.
 */
static int
keycmp(const struct attr_data *attr, const struct attr_data *dir,
       const char *key, size_t len)
{
	const char *p;
	size_t partlen;
	int res;

	do {
		p = memrchr(key, '.', len) ?: key - 1;
		partlen = key + len - p - 1;
		res = strncmp(attr->template->key, p + 1, partlen);
		if (res)
			return res;
		if (attr->template->key[partlen] != '\0')
			return 1;
		attr = attr->parent;
		if (!attr)
			return 1;
		len = p - key;
	} while (p > key);

	return attr->template == dir->template ? 0 : 1;
}

/**  Update a partial hash with an attribute directory path.
 * @param ph   Partial hash state.
 * @param dir  (Leaf) attribute directory attribute.
 *
 * Note that this function's intended use is a lookup under the
 * directory, and the hash includes a terminating dot ("."). This
 * may not be particularly useful for other purposes, but is good
 * enough for the intended one and simplifies the implementation.
 */
static void
path_hash(struct phash *ph, const struct attr_data *dir)
{
	const struct attr_template *tmpl;
	if (dir->parent) {
		path_hash(ph, dir->parent);
		tmpl = dir->template;
		phash_update(ph, tmpl->key, strlen(tmpl->key));
		phash_update(ph, ".", 1);
	}
}

/**  Calculate the hash index of an attribute.
 * @param attr  Attribute.
 * @returns     Desired index in the hash table.
 */
static unsigned
attr_hash_index(const struct attr_data *attr)
{
	const struct attr_template *tmpl;
	struct phash ph;

	phash_init(&ph);
	if (attr->parent)
		path_hash(&ph, attr->parent);
	tmpl = attr->template;
	phash_update(&ph, tmpl->key, strlen(tmpl->key));
	return fold_hash(phash_value(&ph), ATTR_HASH_BITS);
}

/**  Look up a child attribute of a given directory (no fallback).
 * @param dict    Attribute dictionary.
 * @param dir     Directory attribute.
 * @param key     Key name relative to @p dir.
 * @param keylen  Initial portion of @c key to be considered.
 * @returns       Stored attribute or @c NULL if not found.
 */
static struct attr_data *
lookup_dir_attr_no_fallback(struct attr_dict *dict,
			    const struct attr_data *dir,
			    const char *key, size_t keylen)
{
	struct attr_data *d;
	struct phash ph;
	unsigned hash;

	phash_init(&ph);
	path_hash(&ph, dir);
	phash_update(&ph, key, keylen);
	hash = fold_hash(phash_value(&ph), ATTR_HASH_BITS);
	hlist_for_each_entry(d, &dict->attr.table[hash], list)
		if (!keycmp(d, dir, key, keylen))
			return d;
	return NULL;
}

/**  Look up a child attribute of a given directory.
 * @param dict    Attribute dictionary.
 * @param dir     Directory attribute.
 * @param key     Key name relative to @p dir.
 * @param keylen  Initial portion of @c key to be considered.
 * @returns       Stored attribute or @c NULL if not found.
 *
 * If @p key starts with a dot ('.'), the search uses only the specified
 * dictionary, ie. if the search fails, fallback dictionary is not used.
 */
struct attr_data *
lookup_dir_attr(struct attr_dict *dict,
		const struct attr_data *dir,
		const char *key, size_t keylen)
{
	struct phash ph;
	unsigned hash;

	if (*key == '.')
		return lookup_dir_attr_no_fallback(dict, dir, ++key, --keylen);

	phash_init(&ph);
	path_hash(&ph, dir);
	phash_update(&ph, key, keylen);
	hash = fold_hash(phash_value(&ph), ATTR_HASH_BITS);
	do {
		struct attr_data *d;
		hlist_for_each_entry(d, &dict->attr.table[hash], list)
			if (!keycmp(d, dir, key, keylen))
				return d;
		dict = dict->fallback;
	} while (dict);

	return NULL;
}

/**  Look up a child attribute with a known template.
 * @param dir   Directory attribute.
 * @param tmpl  Template of the child.
 * @returns     Child attribute, or @c NULL if not found.
 *
 * Perform a linear search over all children of @c dir, so use
 * this function only if the expected number of children is small
 * (or if you know the child is among the first few children).
 *
 * The result is not ambiguous, because the template specifies the
 * name of the attribute, and duplicate names are not allowed.
 *
 * This function does not check whether @c dir is indeed a dictionary.
 * Bad things will happen if you try to use on another type.
 */
struct attr_data *
lookup_attr_child(const struct attr_data *dir,
		  const struct attr_template *tmpl)
{
	struct attr_data *child;
	for (child = dir->dir; child; child = child->next)
		if (child->template == tmpl)
			return child;
	return NULL;
}

/**  Look up attribute value by name.
 * @param dict    Attribute dictionary.
 * @param key     Key name.
 * @param keylen  Initial portion of @c key to be considered.
 * @returns       Stored attribute or @c NULL if not found.
 */
static struct attr_data*
lookup_attr_part(struct attr_dict *dict,
		 const char *key, size_t keylen)
{
	return lookup_dir_attr(dict, dgattr(dict, GKI_dir_root),
			       key, keylen);
}

/**  Look up attribute data by name.
 * @param dict    Attribute dictionary.
 * @param key     Key name, or @c NULL for the root attribute.
 * @returns       Stored attribute or @c NULL if not found.
 *
 * This function does not check whether an attribute is set, or not.
 */
struct attr_data *
lookup_attr(struct attr_dict *dict, const char *key)
{
	return key
		? lookup_attr_part(dict, key, strlen(key))
		: dgattr(dict, GKI_dir_root);
}

/**  Allocate an attribute from the hash table.
 * @param dict    Attribute dictionary.
 * @param parent  Parent directory, or @c NULL.
 * @param tmpl    Attribute template.
 * @returns       Attribute data, or @c NULL on allocation failure.
 */
static struct attr_data *
alloc_attr(struct attr_dict *dict, struct attr_data *parent,
	   const struct attr_template *tmpl)
{
	struct attr_data *d;
	unsigned hash;

	d = calloc(1, sizeof *d);
	if (!d)
		return d;

	d->parent = parent;
	d->template = tmpl;
	hash = attr_hash_index(d);
	hlist_add_head(&d->list, &dict->attr.table[hash]);

	return d;
}

/**  Discard a value.
 * @param val    Attribute value.
 * @param type   Attribute type.
 * @param flags  Attribute flags.
 *
 * If the value is dynamically allocated, free the associated memory.
 * If the value is refcounted, drop the reference.
 */
static void
discard_value(const kdump_attr_value_t *val, kdump_attr_type_t type,
	      struct attr_flags flags)
{
	switch (type) {
	case KDUMP_NIL:
	case KDUMP_DIRECTORY:
	case KDUMP_NUMBER:
	case KDUMP_ADDRESS:
		/* Value is embedded: Nothing to be done. */
		break;

	case KDUMP_STRING:
		if (flags.dynstr)
			free((void *)val->string);
		break;

	case KDUMP_BITMAP:
		internal_bmp_decref(val->bitmap);
		break;

	case KDUMP_BLOB:
		internal_blob_decref(val->blob);
		break;
	}
}

/**  Discard an attribute's value.
 * @param attr  The attribute whose value is being discarded.
 *
 * Call this function if the attribute data is no longer needed.
 */
static void
discard_attr_value(struct attr_data *attr)
{
	if (!attr_isset(attr))
		return;

	discard_value(attr_value(attr), attr->template->type,
		      attr->flags);
	attr->flags.dynstr = 0;
}

/**  Clear (unset) a single attribute.
 * @param ctx   Dump file object.
 * @param attr  Attribute to be cleared.
 *
 * This function should be used only for attributes without any
 * children.
 */
static void
clear_single_attr(kdump_ctx_t *ctx, struct attr_data *attr)
{
	const struct attr_ops *ops = attr->template->ops;
	if (ops && ops->pre_clear)
		ops->pre_clear(ctx, attr);

	discard_attr_value(attr);
	attr->flags.isset = 0;
}

/**  Clear (unset) any attribute and its children recursively.
 * @param ctx   Dump file object.
 * @param attr  Attribute to be cleared.
 */
void
clear_attr(kdump_ctx_t *ctx, struct attr_data *attr)
{
	struct attr_data *child;

	if (attr->template->type == KDUMP_DIRECTORY)
		for (child = attr->dir; child; child = child->next)
			clear_attr(ctx, child);

	clear_single_attr(ctx, attr);
}

/**  Clear (unset) a volatile attribute and its children recursively.
 * @param ctx   Dump file object.
 * @param attr  Attribute to be cleared.
 * @returns     Non-zero if the entry could not be cleared.
 *
 * This function clears only volatile attributes, i.e. those that were
 * set automatically and should not be preserved when re-opening a dump.
 * Persistent attributes (e.g. those that have been set explicitly) are
 * kept. The complete path to each persistent attributes is also kept.
 */
static unsigned
clear_volatile(kdump_ctx_t *ctx, struct attr_data *attr)
{
	struct attr_data *child;
	unsigned persist;

	persist = attr->flags.persist;
	if (attr->template->type == KDUMP_DIRECTORY)
		for (child = attr->dir; child; child = child->next)
			persist |= clear_volatile(ctx, child);

	if (!persist)
		clear_single_attr(ctx, attr);
	return persist;
}

/**  Clear (unset) all volatile attributes.
 * @param ctx   Dump file object.
 */
void
clear_volatile_attrs(kdump_ctx_t *ctx)
{
	clear_volatile(ctx, gattr(ctx, GKI_dir_root));
}

/**  Deallocate attribute (and its children).
 * @param attr  Attribute data to be deallocated.
 */
void
dealloc_attr(struct attr_data *attr)
{
	if (attr->template->type == KDUMP_DIRECTORY) {
		struct attr_data *next = attr->dir;
		while (next) {
			struct attr_data *child = next;
			next = child->next;
			dealloc_attr(child);
		}
	}

	discard_attr_value(attr);
	if (attr->tflags.dyntmpl)
		free((void*) attr->template);

	hlist_del(&attr->list);
	free(attr);
}

/**  Allocate a new attribute in any directory.
 * @param dict    Attribute dictionary.
 * @param parent  Parent directory. If @c NULL, create a self-owned
 *                attribute (root directory).
 * @param tmpl    Attribute template.
 * @returns       Attribute data, or @c NULL on allocation failure.
 *
 * If an attribute with the same path already exists, reuse the existing
 * attribute, discarding its original value and replacing the template.
 */
struct attr_data *
new_attr(struct attr_dict *dict, struct attr_data *parent,
	 const struct attr_template *tmpl)
{
	struct attr_data *attr;

	if (parent) {
		attr = lookup_dir_attr_no_fallback(
			dict, parent, tmpl->key, strlen(tmpl->key));
		if (attr) {
			discard_attr_value(attr);
			if (attr->tflags.dyntmpl)
				free((void*) attr->template);
			attr->template = tmpl;

			memset(&attr->flags, 0,
			       offsetof(struct attr_data, list) -
			       offsetof(struct attr_data, flags));
			return attr;
		}
	}

	attr = alloc_attr(dict, parent, tmpl);
	if (!attr)
		return attr;

	attr->template = tmpl;

	attr->parent = parent;
	if (parent) {
		attr->next = parent->dir;
		parent->dir = attr;
	}

	return attr;
}

/**  Allocate an attribute template.
 * @param tmpl    Attribute type template.
 * @param key     Key name.
 * @param keylen  Key length (maybe partial).
 * @returns       Newly allocated attribute template, or @c NULL.
 *
 * All template fields except the key name are copied from @p tmpl.
 */
struct attr_template *
alloc_attr_template(const struct attr_template *tmpl,
		    const char *key, size_t keylen)
{
	struct attr_template *ret;

	ret = malloc(sizeof *ret + keylen + 1);
	if (ret) {
		char *retkey;

		*ret = *tmpl;
		retkey = (char*) (ret + 1);
		memcpy(retkey, key, keylen);
		retkey[keylen] = '\0';
		ret->key = retkey;
	}
	return ret;
}

/** Create an attribute including full path.
 * @param dict    Attribute dictionary.
 * @param dir     Base directory.
 * @param path    Path under @p dir.
 * @param pathlen Length of @p path (maybe partial).
 * @param atmpl   Attribute template.
 * @returns       Attribute data, or @c NULL on allocation failure.
 *
 * Look up the attribute @p path under @p dir. If the attribute does not
 * exist yet, create it with type @p type. If @p path contains dots, then
 * all path elements are also created as necessary.
 */
struct attr_data *
create_attr_path(struct attr_dict *dict, struct attr_data *dir,
		 const char *path, size_t pathlen,
		 const struct attr_template *atmpl)
{
	const char *p, *endp, *endpath;
	struct attr_data *attr;
	struct attr_template *tmpl;

	endp = endpath = path + pathlen;
	while (! (attr = lookup_dir_attr(dict, dir, path, endp - path)) )
		if (! (endp = memrchr(path, '.', endp - path)) ) {
			endp = path - 1;
			attr = dir;
			break;
		}

	while (endp && endp != endpath) {
		p = endp + 1;
		endp = memchr(p, '.', endpath - p);

		tmpl = endp
			? alloc_attr_template(&dir_template, p, endp - p)
			: alloc_attr_template(atmpl, p, endpath - p);
		if (!tmpl)
			return NULL;
		attr = new_attr(dict, attr, tmpl);
		if (!attr) {
			free(tmpl);
			return NULL;
		}
		attr->tflags.dyntmpl = 1;
	}

	return attr;
}

/** Copy attribute data.
 * @param dest  Destination attribute.
 * @param src   Source attribute.
 * @returns     @c true on success, @c false on allocation failure
 */
static bool
copy_data(struct attr_data *dest, const struct attr_data *src)
{
	dest->flags.isset = 1;
	dest->flags.persist = src->flags.persist;

	switch (src->template->type) {
	case KDUMP_DIRECTORY:
		return true;

	case KDUMP_NUMBER:
	case KDUMP_ADDRESS:
		dest->val = *attr_value(src);
		return true;

	case KDUMP_STRING:
		dest->val.string = strdup(attr_value(src)->string);
		if (!dest->val.string)
			return false;
		dest->flags.dynstr = true;
		return true;

	case KDUMP_BITMAP:
	case KDUMP_BLOB:
		/* Not yet implemented */

	case KDUMP_NIL:		/* should not happen */
	default:
		return false;
	}
}

/** Clone an attribute.
 * @param dict    Destination attribute dictionary.
 * @param dir     Target attribute directory.
 * @param orig    Attribute to be cloned.
 * @returns       Attribute data, or @c NULL on allocation failure.
 */
static struct attr_data *
clone_attr(struct attr_dict *dict, struct attr_data *dir,
	   struct attr_data *orig)
{
	struct attr_data *newattr;

	newattr = new_attr(dict, dir, orig->template);
	if (!newattr)
		return NULL;

	if (attr_isset(orig) && !copy_data(newattr, orig))
		return NULL;

	/* If this is a global attribute, update global_attrs[] */
	if (newattr->template >= global_keys &&
	    newattr->template < &global_keys[NR_GLOBAL_ATTRS]) {
		enum global_keyidx idx = newattr->template - global_keys;
		dict->global_attrs[idx] = newattr;
	}

	return newattr;
}

/** Clone an attribute subtree.
 * @param dict    Destination attribute dictionary.
 * @param dir     Target attribute directory.
 * @param orig    Attribute directory to be cloned.
 * @returns       @c true on success, @c false on allocation failure.
 */
static bool
clone_subtree(struct attr_dict *dict, struct attr_data *dir,
	      struct attr_data *orig)
{
	for (orig = orig->dir; orig; orig = orig->next) {
		struct attr_data *newattr;
		newattr = clone_attr(dict, dir, orig);
		if (!newattr)
			return false;
		if (orig->template->type == KDUMP_DIRECTORY &&
		    !clone_subtree(dict, newattr, orig))
			return false;
	}

	return true;
}

/** Clone an attribute including full path.
 * @param dict    Destination attribute dictionary.
 * @param orig    Attribute to be cloned.
 * @returns       Attribute data, or @c NULL on allocation failure.
 *
 * Make a copy of @p attr in the target dictionary @p dict. Make sure
 * that all path components of the new target are also cloned in the
 * target dictionary.
 */
struct attr_data *
clone_attr_path(struct attr_dict *dict, struct attr_data *orig)
{
	char *path;
	const char *p, *endp, *endpath;
	size_t pathlen;
	struct attr_data *attr;
	struct attr_data *base;

	pathlen = attr_pathlen(orig) + 1;
	path = alloca(pathlen + 1);
	*path = '.';
	make_attr_path(orig, path + pathlen);

	endp = endpath = path + pathlen;
	while (! (attr = lookup_attr_part(dict, path, endp - path)) )
		if (! (endp = memrchr(path + 1, '.', endp - path - 1)) ) {
			endp = path;
			attr = dgattr(dict, GKI_dir_root);
			break;
		}

	base = attr;
	while (endp && endp != endpath) {
		struct attr_data *newattr;

		p = endp + 1;
		endp = memchr(p, '.', endpath - p);
		orig = endp
			? lookup_attr_part(dict, path + 1, endp - path - 1)
			: lookup_attr(dict, path + 1);
		newattr = clone_attr(dict, attr, orig);
		if (!newattr)
			goto err;
		attr = newattr;
	}

	if (orig->template->type == KDUMP_DIRECTORY &&
	    !clone_subtree(dict, attr, orig))
		goto err;

	return attr;

 err:
	while (attr != base) {
		struct attr_data *next = attr->parent;
		dealloc_attr(attr);
		attr = next;
	}
	return NULL;
}

/**  Instantiate a directory path.
 * @param attr  Leaf attribute.
 * @returns     The newly instantiated attribute,
 *              or @c NULL on allocation failure.
 *
 * Inititalize all paths up the hierarchy for the (leaf) directory
 * denoted by @c tmpl.
 */
static void
instantiate_path(struct attr_data *attr)
{
	while (!attr_isset(attr)) {
		attr->flags.isset = 1;
		if (!attr->parent)
			break;
		attr = attr->parent;
	}
}

/**  Free an attribute dictionary.
 * @param dict  Attribute dictionary.
 */
void
attr_dict_free(struct attr_dict *dict)
{
	if (dict->shared->arch_ops && dict->shared->arch_ops->attr_cleanup)
		dict->shared->arch_ops->attr_cleanup(dict);
	if (dict->shared->ops && dict->shared->ops->attr_cleanup)
		dict->shared->ops->attr_cleanup(dict);

	dealloc_attr(dgattr(dict, GKI_dir_root));

	if (dict->fallback)
		attr_dict_decref(dict->fallback);
	shared_decref_locked(dict->shared);
	free(dict);
}

/**  Initialize global attributes
 * @param shared  Shared data of a dump file object.
 * @returns       Attribute directory, or @c NULL on allocation failure.
 */
struct attr_dict *
attr_dict_new(struct kdump_shared *shared)
{
	struct attr_dict *dict;
	enum global_keyidx i;

	dict = calloc(1, sizeof(struct attr_dict));
	if (!dict)
		return NULL;

	dict->refcnt = 1;

	for (i = 0; i < NR_GLOBAL_ATTRS; ++i) {
		const struct attr_template *tmpl = &global_keys[i];
		struct attr_data *attr, *parent;

		parent = dict->global_attrs[tmpl->parent_key];
		attr = new_attr(dict, parent, tmpl);
		if (!attr)
			return NULL;
		dict->global_attrs[i] = attr;

		if (i >= GKI_static_first && i <= GKI_static_last) {
			attr->flags.indirect = 1;
			attr->pval = static_attr_value(shared, i);
		}
	}

	dict->shared = shared;
	shared_incref_locked(dict->shared);

	return dict;
}

/**  Clone an attribute dictionary.
 * @param orig  Dictionary to be cloned.
 * @returns     Attribute directory, or @c NULL on allocation failure.
 *
 * The new dictionary's root directory is initiailized as an indirect
 * attribute pointing to the original dictionary.
 */
struct attr_dict *
attr_dict_clone(struct attr_dict *orig)
{
	struct attr_dict *dict;
	struct attr_data *rootdir;

	dict = calloc(1, sizeof(struct attr_dict));
	if (!dict)
		return NULL;
	dict->refcnt = 1;

	memcpy(dict->global_attrs, orig->global_attrs,
	       sizeof(orig->global_attrs));

	rootdir = new_attr(dict, NULL, &global_keys[GKI_dir_root]);
	if (!rootdir) {
		free(dict);
		return NULL;
	}
	dict->global_attrs[GKI_dir_root] = rootdir;

	dict->fallback = orig;
	attr_dict_incref(dict->fallback);
	dict->shared = orig->shared;
	shared_incref_locked(dict->shared);

	return dict;
}

/**  Check whether an attribute has a given value.
 * @param attr    Attribute data.
 * @param newval  Checked value.
 * @returns       Non-zero if attribute already has this value,
 *                zero otherwise.
 */
static int
attr_has_value(struct attr_data *attr, kdump_attr_value_t newval)
{
	const kdump_attr_value_t *oldval = attr_value(attr);

	if (!attr_isset(attr))
		return 0;

	switch (attr->template->type) {
	case KDUMP_DIRECTORY:
		return 1;

	case KDUMP_NUMBER:
		return oldval->number == newval.number;

	case KDUMP_ADDRESS:
		return oldval->address == newval.address;

	case KDUMP_STRING:
		return oldval->string == newval.string ||
			!strcmp(oldval->string, newval.string);

	case KDUMP_BITMAP:
		return oldval->bitmap == newval.bitmap;

	case KDUMP_BLOB:
		return oldval->blob == newval.blob;

	case KDUMP_NIL:
	default:
		return 0;	/* Should not happen */
	}
}

/**  Set an attribute of a dump file object.
 * @param ctx      Dump file object.
 * @param attr     Attribute data.
 * @param flags    New attribute value flags.
 * @param pval     Pointer to new attribute value (ignored for directories).
 * @returns        Error status.
 *
 * Note that the @c flags.indirect has a slightly different meaning:
 *
 * - If the flag is set, @p pval is set as the value location for @p attr.
 * - If the flag is clear, the value of @p attr is changed, but the value
 *   of @c attr->flags.indirect is left unmodified.
 *
 * The idea is that you set @c flags.indirect, if @p pval should become
 * the new indirect value of @p attr. If you want to modify only the value
 * of @p attr, leave @c flags.indirect clear.
 *
 * The attribute object takes over ownership of the new value. If the
 * attribute type is refcounted, then the reference is stolen from the
 * caller. This is true even if the function fails and returns an error
 * status. In other words, you must increase the reference count to the new
 * value before calling this function if you use that object in the error
 * path.
 */
kdump_status
set_attr(kdump_ctx_t *ctx, struct attr_data *attr,
	 struct attr_flags flags, kdump_attr_value_t *pval)
{
	int skiphooks = attr_has_value(attr, *pval);
	kdump_status res;

	if (!skiphooks) {
		const struct attr_ops *ops = attr->template->ops;
		if (ops && ops->pre_set &&
		    (res = ops->pre_set(ctx, attr, pval)) != KDUMP_OK) {
			flags.indirect = 0;
			discard_value(pval, attr->template->type, flags);
			return res;
		}
	}

	instantiate_path(attr->parent);

	if (attr->template->type != KDUMP_DIRECTORY) {
		discard_attr_value(attr);

		if (flags.indirect)
			attr->pval = pval;
		else if (attr->flags.indirect) {
			flags.indirect = 1;
			*attr->pval = *pval;
		} else
			attr->val = *pval;
	}
	flags.isset = 1;
	attr->flags = flags;

	if (!skiphooks) {
		const struct attr_ops *ops = attr->template->ops;
		if (ops && ops->post_set &&
		    (res = ops->post_set(ctx, attr)) != KDUMP_OK)
			return res;
	}

	return KDUMP_OK;
}

/**  Set a numeric attribute of a dump file object.
 * @param ctx      Dump file object.
 * @param attr     Attribute data.
 * @param flags    New attribute value flags.
 * @param num      Key value (numeric).
 * @returns        Error status.
 */
kdump_status
set_attr_number(kdump_ctx_t *ctx, struct attr_data *attr,
		struct attr_flags flags, kdump_num_t num)
{
	kdump_attr_value_t val;

	val.number = num;
	return set_attr(ctx, attr, flags, &val);
}

/**  Set an address attribute of a dump file object.
 * @param ctx      Dump file object.
 * @param attr     Attribute data.
 * @param flags    New attribute value flags.
 * @param addr     Key value (address).
 * @returns        Error status.
 */
kdump_status
set_attr_address(kdump_ctx_t *ctx, struct attr_data *attr,
		 struct attr_flags flags, kdump_addr_t addr)
{
	kdump_attr_value_t val;

	val.address = addr;
	return set_attr(ctx, attr, flags, &val);
}

/**  Set a string attribute's value.
 * @param ctx      Dump file object.
 * @param attr     An attribute string.
 * @param flags    New attribute value flags.
 * @param str      New string value.
 * @returns        Error status.
 */
kdump_status
set_attr_string(kdump_ctx_t *ctx, struct attr_data *attr,
		struct attr_flags flags, const char *str)
{
	char *dynstr = strdup(str);
	kdump_attr_value_t val;

	if (!dynstr)
		return set_error(ctx, KDUMP_ERR_SYSTEM,
				 "Cannot allocate string");

	val.string = dynstr;
	flags.dynstr = 1;
	return set_attr(ctx, attr, flags, &val);
}

/**  Set a string attribute's value to a string of a known size.
 * @param ctx      Dump file object.
 * @param attr     An attribute string.
 * @param flags    New attribute value flags.
 * @param str      New string value.
 * @param len      Length of the new value.
 * @returns        Error status.
 */
kdump_status
set_attr_sized_string(kdump_ctx_t *ctx, struct attr_data *attr,
		      struct attr_flags flags, const char *str, size_t len)
{
	size_t dynlen;
	char *dynstr;
	kdump_attr_value_t val;

	dynlen = len;
	if (!len || str[len-1] != '\0')
		++dynlen;
	dynstr = ctx_malloc(dynlen, ctx, "sized string");
	if (!dynstr)
		return KDUMP_ERR_SYSTEM;
	memcpy(dynstr, str, len);
	dynstr[dynlen-1] = '\0';

	val.string = dynstr;
	flags.dynstr = 1;
	return set_attr(ctx, attr, flags, &val);
}

/**  Set a static string attribute of a dump file object.
 * @param ctx      Dump file object.
 * @param attr     Attribute data.
 * @param flags    New attribute value flags.
 * @param str      Key value (static string).
 * @returns        Error status.
 */
kdump_status
set_attr_static_string(kdump_ctx_t *ctx, struct attr_data *attr,
		       struct attr_flags flags, const char *str)
{
	kdump_attr_value_t val;

	val.string = str;
	return set_attr(ctx, attr, flags, &val);
}

/**  Add a template override to an attribute.
 * @param attr      Attribute data.
 * @param override  Override definition.
 */
void
attr_add_override(struct attr_data *attr, struct attr_override *override)
{
	const struct attr_template *tmpl = attr->template;

	if (tmpl->ops)
		override->ops = *tmpl->ops;
	else
		memset(&override->ops, 0, sizeof override->ops);

	override->template.key = tmpl->key;
	override->template.parent = attr->template;
	override->template.type = tmpl->type;
	override->template.override = 1;
	override->template.ops = &override->ops;

	attr->template = &override->template;
}

/**  Remove a template override from an attribute.
 * @param attr      Attribute data.
 * @param override  Override definition to be removed.
 */
void
attr_remove_override(struct attr_data *attr, struct attr_override *override)
{
	const struct attr_template *tmpl, **pprev;
	pprev = &attr->template;
	do {
		tmpl = *pprev;
		if (tmpl == &override->template) {
			*pprev = tmpl->parent;
			break;
		}
		pprev = &((struct attr_template*)tmpl)->parent;
	} while (tmpl->override);
}

DEFINE_ALIAS(get_attr);

kdump_status
kdump_get_attr(kdump_ctx_t *ctx, const char *key, kdump_attr_t *valp)
{
	struct attr_data *d;
	kdump_status ret;

	clear_error(ctx);
	rwlock_rdlock(&ctx->shared->lock);

	d = lookup_attr(ctx->dict, key);
	if (!d) {
		ret = set_error(ctx, KDUMP_ERR_NOKEY, "No such key");
		goto out;
	}
	if (!attr_isset(d)) {
		ret = set_error(ctx, KDUMP_ERR_NODATA, "Key has no value");
		goto out;
	}
	ret = attr_revalidate(ctx, d);
	if (ret != KDUMP_OK) {
		ret = set_error(ctx, ret, "Value cannot be revalidated");
		goto out;
	}

	valp->type = d->template->type;
	valp->val = *attr_value(d);
	ret = KDUMP_OK;

 out:
	rwlock_unlock(&ctx->shared->lock);
	return ret;
}

kdump_status
kdump_get_typed_attr(kdump_ctx_t *ctx, const char *key, kdump_attr_t *valp)
{
	kdump_attr_type_t type = valp->type;
	kdump_status ret;

	ret = internal_get_attr(ctx, key, valp);
	if (ret != KDUMP_OK)
		return ret;

	if (valp->type != type)
		return set_error(ctx, KDUMP_ERR_INVALID,
				 "Attribute type mismatch");

	return KDUMP_OK;
}

/**  Set an attribute value with type check.
 * @param ctx   Dump file object.
 * @param attr  Attribute to be modified.
 * @param valp  New value for the attribute.
 */
static kdump_status
check_set_attr(kdump_ctx_t *ctx, struct attr_data *attr,
	       const kdump_attr_t *valp)
{
	kdump_attr_value_t val;

	if (valp->type == KDUMP_NIL) {
		clear_attr(ctx, attr);
		return KDUMP_OK;
	}

	if (valp->type != attr->template->type)
		return set_error(ctx, KDUMP_ERR_INVALID, "Type mismatch");

	if (valp->type == KDUMP_STRING)
		return set_attr_string(ctx, attr, ATTR_PERSIST,
				       valp->val.string);

	val = valp->val;
	return set_attr(ctx, attr, ATTR_PERSIST, &val);
}

kdump_status
kdump_set_attr(kdump_ctx_t *ctx, const char *key,
	       const kdump_attr_t *valp)
{
	struct attr_data *d;
	kdump_status ret;

	clear_error(ctx);
	rwlock_wrlock(&ctx->shared->lock);

	d = lookup_attr(ctx->dict, key);
	if (!d) {
		ret = set_error(ctx, KDUMP_ERR_NODATA, "No such key");
		goto out;
	}

	ret = check_set_attr(ctx, d, valp);

 out:
	rwlock_unlock(&ctx->shared->lock);
	return ret;
}

/**  Convert attribute data to an attribute reference.
 * @param[out] ref   Attribute reference.
 * @param[in]  attr  Attribute data.
 */
static inline void
mkref(kdump_attr_ref_t *ref, struct attr_data *attr)
{
	ref->_ptr = attr;
}

/**  Convert an attribute reference to attribute data.
 * @param ref  Attribute reference.
 * @returns    Attribute data.
 */
static inline struct attr_data *
ref_attr(const kdump_attr_ref_t *ref)
{
	return ref->_ptr;
}

kdump_status
kdump_attr_ref(kdump_ctx_t *ctx, const char *key, kdump_attr_ref_t *ref)
{
	struct attr_data *d;

	clear_error(ctx);

	rwlock_rdlock(&ctx->shared->lock);
	d = lookup_attr(ctx->dict, key);
	rwlock_unlock(&ctx->shared->lock);
	if (!d)
		return set_error(ctx, KDUMP_ERR_NOKEY, "No such key");

	mkref(ref, d);
	return KDUMP_OK;
}

kdump_status
kdump_sub_attr_ref(kdump_ctx_t *ctx, const kdump_attr_ref_t *base,
		   const char *subkey, kdump_attr_ref_t *ref)
{
	struct attr_data *dir, *attr;

	clear_error(ctx);

	dir = ref_attr(base);
	rwlock_rdlock(&ctx->shared->lock);
	attr = lookup_dir_attr(ctx->dict, dir, subkey, strlen(subkey));
	rwlock_unlock(&ctx->shared->lock);
	if (!attr)
		return set_error(ctx, KDUMP_ERR_NOKEY, "No such key");

	mkref(ref, attr);
	return KDUMP_OK;
}

void
kdump_attr_unref(kdump_ctx_t *ctx, kdump_attr_ref_t *ref)
{
	clear_error(ctx);
}

kdump_attr_type_t
kdump_attr_ref_type(kdump_attr_ref_t *ref)
{
	return ref_attr(ref)->template->type;
}

int
kdump_attr_ref_isset(kdump_attr_ref_t *ref)
{
	return attr_isset(ref_attr(ref));
}

kdump_status
kdump_attr_ref_get(kdump_ctx_t *ctx, const kdump_attr_ref_t *ref,
		   kdump_attr_t *valp)
{
	struct attr_data *d = ref_attr(ref);
	kdump_status ret;

	clear_error(ctx);
	rwlock_rdlock(&ctx->shared->lock);

	if (!attr_isset(d)) {
		ret = set_error(ctx, KDUMP_ERR_NODATA, "Key has no value");
		goto out;
	}
	ret = attr_revalidate(ctx, d);
	if (ret != KDUMP_OK) {
		ret = set_error(ctx, ret, "Value cannot be revalidated");
		goto out;
	}

	valp->type = d->template->type;
	valp->val = *attr_value(d);
	ret = KDUMP_OK;

 out:
	rwlock_unlock(&ctx->shared->lock);
	return ret;
}

kdump_status
kdump_attr_ref_set(kdump_ctx_t *ctx, kdump_attr_ref_t *ref,
		   const kdump_attr_t *valp)
{
	kdump_status ret;

	clear_error(ctx);
	rwlock_wrlock(&ctx->shared->lock);

	ret = check_set_attr(ctx, ref_attr(ref), valp);

	rwlock_unlock(&ctx->shared->lock);
	return ret;
}

kdump_status
kdump_set_sub_attr(kdump_ctx_t *ctx, const kdump_attr_ref_t *base,
		   const char *subkey, const kdump_attr_t *valp)
{
	struct attr_data *dir, *attr;
	kdump_status ret;

	clear_error(ctx);
	dir = ref_attr(base);
	rwlock_wrlock(&ctx->shared->lock);

	attr = lookup_dir_attr(ctx->dict, dir, subkey, strlen(subkey));
	ret = attr
		? check_set_attr(ctx, attr, valp)
		: set_error(ctx, KDUMP_ERR_NOKEY, "No such key");

	rwlock_unlock(&ctx->shared->lock);
	return ret;
}

static kdump_status
set_iter_pos(kdump_attr_iter_t *iter, struct attr_data *attr)
{
	while (attr && !attr_isset(attr))
		attr = attr->next;

	iter->key = attr ? attr->template->key : NULL;
	mkref(&iter->pos, attr);
	return KDUMP_OK;
}

/**  Get an attribute iterator by attribute data.
 * @param      ctx   Dump file object.
 * @param[in]  attr  Attribute directory data.
 * @param[out] iter  Attribute iterator.
 * @returns          Error status.
 *
 * This is the common implementation of @ref kdump_attr_iter_start
 * and @ref kdump_attr_ref_iter_start, which takes an attribute data
 * pointer as argument.
 */
static kdump_status
attr_iter_start(kdump_ctx_t *ctx, const struct attr_data *attr,
		kdump_attr_iter_t *iter)
{
	if (!attr_isset(attr))
		return set_error(ctx, KDUMP_ERR_NODATA, "Key has no value");
	if (attr->template->type != KDUMP_DIRECTORY)
		return set_error(ctx, KDUMP_ERR_INVALID,
				 "Path is a leaf attribute");

	return set_iter_pos(iter, attr->dir);
}

kdump_status
kdump_attr_iter_start(kdump_ctx_t *ctx, const char *path,
		      kdump_attr_iter_t *iter)
{
	struct attr_data *d;
	kdump_status ret;

	clear_error(ctx);
	rwlock_rdlock(&ctx->shared->lock);

	d = lookup_attr(ctx->dict, path);
	if (d)
		ret = attr_iter_start(ctx, d, iter);
	else
		ret = set_error(ctx, KDUMP_ERR_NOKEY, "No such path");

	rwlock_unlock(&ctx->shared->lock);
	return ret;
}

kdump_status
kdump_attr_ref_iter_start(kdump_ctx_t *ctx, const kdump_attr_ref_t *ref,
			  kdump_attr_iter_t *iter)
{
	kdump_status ret;
	clear_error(ctx);
	rwlock_rdlock(&ctx->shared->lock);
	ret = attr_iter_start(ctx, ref_attr(ref), iter);
	rwlock_unlock(&ctx->shared->lock);
	return ret;
}

kdump_status
kdump_attr_iter_next(kdump_ctx_t *ctx, kdump_attr_iter_t *iter)
{
	struct attr_data *d;
	kdump_status ret;

	clear_error(ctx);
	rwlock_rdlock(&ctx->shared->lock);

	d = ref_attr(&iter->pos);
	if (d)
		ret = set_iter_pos(iter, d->next);
	else
		ret = set_error(ctx, KDUMP_ERR_INVALID, "End of iteration");

	rwlock_unlock(&ctx->shared->lock);
	return ret;
}

void
kdump_attr_iter_end(kdump_ctx_t *ctx, kdump_attr_iter_t *iter)
{
	clear_error(ctx);
}

/**  Use a map to choose an attribute by current OS type.
 * @param      ctx   Dump file object.
 * @param[in]  name  Attribute name under the OS directory.
 * @param[out] attr  Attribute data (set on success).
 * @returns          Error status.
 */
kdump_status
ostype_attr(kdump_ctx_t *ctx, const char *name, struct attr_data **attr)
{
	struct attr_data *d, *a;
	kdump_status status;

	/* Get OS directory attribute */
	if (ctx->xlat->osdir == NR_GLOBAL_ATTRS)
		return set_error(ctx, KDUMP_ERR_NODATA,
				 "OS type is not set");
	d = gattr(ctx, ctx->xlat->osdir);

	/* Get attribute under the OS directory. */
	a = lookup_dir_attr(ctx->dict, d, name, strlen(name));
	if (!a || !attr_isset(a))
		return set_error(ctx, KDUMP_ERR_NODATA,
				 "%s.%s is not set",
				 d->template->key, name);
	status = attr_revalidate(ctx, a);
	if (status != KDUMP_OK)
		return set_error(ctx, status,
				 "Cannot get %s.%s",
				 d->template->key, name);

	*attr = a;
	return KDUMP_OK;
}
