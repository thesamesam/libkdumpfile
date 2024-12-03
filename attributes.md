Attributes								{#attributes}
==========

All meta-information about a dump file is stored in attributes. All attributes
form a tree-like dictionary, where every attribute is placed according to its
path from the root node. The textual representation of this path, where path
elements are delimited by dots, is called _attribute key_.

Some attributes are present in every [kdump_ctx_t] object, although not
necessarily set. They are called _global attributes_. Other attributes may be
created by the library as needed, e.g. CPU registers. It is not possible to
create a new attribute with an arbitrary path through the public API.

Every attribute has a type and an optional value. The type and value can be
queried with `kdump_get_attr`. If the attribute is unset (i.e. it does not
have a value), this function returns `KDUMP_ERR_NODATA`. Attribute value can
be set (or unset) with `kdump_set_attr`, but the type of an attribute cannot
be changed.

Lifetime
--------

Trivial types do not implement reference counting. The attribute value is
always copied. Following types are trivial:
- `KDUMP_NUMBER`
- `KDUMP_ADDRESS`
- `KDUMP_STRING`

Other attribute types refer to objects, and reference counting is used to make
sure that the object does not unexpectedly disappear if the attribute value is
(directly or indirectly) changed. Following types are reference-counted:
- `KDUMP_BITMAP`
- `KDUMP_BLOB`

For historical reasons, the simple attribute API (`kdump_get_attr`,
`kdump_get_attr` and friends) does not increase the reference count of the
returned data. User of this API must not modify the context object while
making use of the returned attribute value. On the other hand, they don't have
to do anything when they are finished.

When a value is returned by the reference-based API (`kdump_attr_ref_get`),
reference count is increased (or a new dynamic string is allocated). Users of
the attribute reference API should release the underlying resources with a
call to `kdump_attr_discard`.

When a new value is set with `kdump_set_attr`, attributes with a trivial type
make a copy of the new value, and attributes with a reference-counted type
take ownership of the reference from the caller.

Well-known Attributes
---------------------

Some attribute keys are available as macros. The intention is to spot mistakes
earlier: A typo in a macro name is detected at build time, whereas a typo in a
key string is not detected until run time.

Implementation
--------------

Internally, there are two types of attributes:

- global, and
- allocated.

Global attributes are allocated at dictionary initialization time, i.e. they
exist in every dictionary object (albeit possibly unset). They can be looked
up directly using a value from [enum global_keyidx] (identifiers with a `GKI_`
prefix) as an index into the `global_attrs` fixed-size array in
[struct attr_dict]. They can also be looked up through the hash table, like
any other attribute, but that's less efficient, of course.

The values of some global attributes are assumed to be always available and
valid. These values are in fact stored in [struct kdump_shared] and can be
accessed from here with zero overhead by inline helper functions. These
attributes are called _static_ and are used for frequently accessed values,
such as `arch.byte_order` or `file.pagemap`.

It is possible to associate attribute operations with static attributes.
However, the `revalidate` hook is not called when the value is accessed
internally by the library using the `get_` or `sget_` helper functions.
It is usually a bug to set `flags.invalid` for a static attribute.

All non-global attributes are dynamically allocated by `new_attr`, and there
is nothing special about them.

Volatile and Persistent Attributes
----------------------------------

Volatile attributes are cleared when a file format probe returns an error.
Persistent attributes are kept. All attributes set through the public API are
persistent. This implementation detail allows to set attribute values while
probing a file format and remove those attributes automatically if it turns
out to be a different file type.

The difference between volatile and persistent attributes is not visible in
the public API.

[kdump_ctx_t]: @ref kdump_ctx_t
[struct kdump_shared]: @ref kdump_shared
[enum global_keyidx]: @ref global_keyidx
[struct attr_dict]: @ref attr_dict
