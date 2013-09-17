/* Masstree
 * Eddie Kohler, Yandong Mao, Robert Morris
 * Copyright (c) 2012-2013 President and Fellows of Harvard College
 * Copyright (c) 2012-2013 Massachusetts Institute of Technology
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, subject to the conditions
 * listed in the Masstree LICENSE file. These conditions include: you must
 * preserve this copyright notice, and you cannot mention the copyright
 * holders in advertising related to the Software without their permission.
 * The Software is provided WITHOUT ANY WARRANTY, EXPRESS OR IMPLIED. This
 * notice is a summary of the Masstree LICENSE file; the license in that file
 * is legally binding.
 */
#ifndef MASSTREE_STRUCT_HH
#define MASSTREE_STRUCT_HH
#include "masstree.hh"
#include "nodeversion.hh"
#include "stringbag.hh"
#include "mtcounters.hh"
#include "timestamp.hh"
namespace Masstree {

template <typename P>
struct make_nodeversion {
    typedef typename mass::conditional<P::concurrent,
				       nodeversion,
				       singlethreaded_nodeversion>::type type;
};

template <typename P>
struct make_prefetcher {
    typedef typename mass::conditional<P::prefetch,
				       value_prefetcher<typename P::value_type>,
				       do_nothing>::type type;
};

template <typename P>
class node_base : public make_nodeversion<P>::type {
  public:
    static constexpr bool concurrent = P::concurrent;
    static constexpr int nikey = 1;
    typedef leaf<P> leaf_type;
    typedef internode<P> internode_type;
    typedef node_base<P> base_type;
    typedef typename P::value_type value_type;
    typedef leafvalue<P> leafvalue_type;
    typedef typename P::ikey_type ikey_type;
    typedef key<ikey_type> key_type;
    typedef typename make_nodeversion<P>::type nodeversion_type;
    typedef typename P::threadinfo_type threadinfo;

    node_base(bool isleaf)
	: nodeversion_type(isleaf) {
    }

    int size() const {
	if (this->isleaf())
	    return static_cast<const leaf_type*>(this)->size();
	else
	    return static_cast<const internode_type*>(this)->size();
    }

    inline base_type* parent() const {
        // almost always an internode
	if (this->isleaf())
	    return static_cast<const leaf_type*>(this)->parent_;
	else
	    return static_cast<const internode_type*>(this)->parent_;
    }
    static inline base_type* parent_for_layer_root(base_type* higher_layer) {
        (void) higher_layer;
        return 0;
    }
    static inline bool parent_exists(base_type* p) {
        return p != 0;
    }
    inline bool has_parent() const {
        return parent_exists(parent());
    }
    inline internode_type* locked_parent(threadinfo& ti) const;
    inline void set_parent(base_type* p) {
	if (this->isleaf())
	    static_cast<leaf_type*>(this)->parent_ = p;
	else
	    static_cast<internode_type*>(this)->parent_ = p;
    }
    inline base_type* unsplit_ancestor() const {
	base_type* x = const_cast<base_type*>(this), *p;
	while (x->has_split() && (p = x->parent()))
	    x = p;
	return x;
    }

    void prefetch_full() const {
	for (int i = 0; i < std::min(16 * std::min(P::leaf_width, P::internode_width) + 1, 4 * 64); i += 64)
	    ::prefetch((const char *) this + i);
    }

    void print(FILE* f, const char* prefix, int indent, int kdepth);
};

template <typename P>
class internode : public node_base<P> {
  public:
    static constexpr int width = P::internode_width;
    typedef typename node_base<P>::nodeversion_type nodeversion_type;
    typedef key<typename P::ikey_type> key_type;
    typedef typename P::ikey_type ikey_type;
    typedef typename key_bound<width, P::bound_method>::type bound_type;
    typedef typename P::threadinfo_type threadinfo;

    uint8_t nkeys_;
    ikey_type ikey0_[width];
    node_base<P>* child_[width + 1];
    node_base<P>* parent_;
    kvtimestamp_t created_at_[P::debug_level > 0];

    internode()
	: node_base<P>(false), nkeys_(0), parent_() {
    }

    static internode<P>* make(threadinfo& ti) {
	void* ptr = ti.pool_allocate(sizeof(internode<P>),
                                     memtag_masstree_internode);
	internode<P>* n = new(ptr) internode<P>;
	assert(n);
	if (P::debug_level > 0)
	    n->created_at_[0] = ti.operation_timestamp();
	return n;
    }

    int size() const {
	return nkeys_;
    }

    key_type get_key(int p) const {
	return key_type(ikey0_[p]);
    }
    ikey_type ikey(int p) const {
	return ikey0_[p];
    }
    inline int stable_last_key_compare(const key_type& k, nodeversion_type v,
                                       threadinfo& ti) const;

    void assign(int p, ikey_type ikey, node_base<P>* child) {
	child->set_parent(this);
	child_[p + 1] = child;
	ikey0_[p] = ikey;
    }

    void shift_from(int p, const internode<P>* x, int xp, int n) {
	masstree_precondition(x != this);
	if (n) {
	    memcpy(ikey0_ + p, x->ikey0_ + xp, sizeof(ikey0_[0]) * n);
	    memcpy(child_ + p + 1, x->child_ + xp + 1, sizeof(child_[0]) * n);
	}
    }
    void shift_up(int p, int xp, int n) {
	memmove(ikey0_ + p, ikey0_ + xp, sizeof(ikey0_[0]) * n);
	for (node_base<P> **a = child_ + p + n, **b = child_ + xp + n; n; --a, --b, --n)
	    *a = *b;
    }
    void shift_down(int p, int xp, int n) {
	memmove(ikey0_ + p, ikey0_ + xp, sizeof(ikey0_[0]) * n);
	for (node_base<P> **a = child_ + p + 1, **b = child_ + xp + 1; n; ++a, ++b, --n)
	    *a = *b;
    }

    void prefetch() const {
	for (int i = 64; i < std::min(16 * width + 1, 4 * 64); i += 64)
	    ::prefetch((const char *) this + i);
    }

    void print(FILE* f, const char* prefix, int indent, int kdepth);

    void deallocate(threadinfo& ti) {
	ti.pool_deallocate(this, sizeof(*this), memtag_masstree_internode);
    }
    void deallocate_rcu(threadinfo& ti) {
	ti.pool_deallocate_rcu(this, sizeof(*this), memtag_masstree_internode);
    }
};

template <typename P>
class leafvalue {
  public:
    typedef typename P::value_type value_type;
    typedef typename make_prefetcher<P>::type prefetcher_type;

    leafvalue() {
    }
    leafvalue(value_type v) {
	u_.v = v;
    }
    leafvalue(node_base<P>* n) {
	u_.x = reinterpret_cast<uintptr_t>(n);
    }

    static leafvalue<P> make_empty() {
	return leafvalue<P>(value_type());
    }

    typedef bool (leafvalue<P>::*unspecified_bool_type)() const;
    operator unspecified_bool_type() const {
	return u_.x ? &leafvalue<P>::empty : 0;
    }
    bool empty() const {
	return !u_.x;
    }

    value_type value() const {
	return u_.v;
    }
    value_type& value() {
	return u_.v;
    }

    node_base<P>* layer() const {
	return reinterpret_cast<node_base<P>*>(u_.x);
    }

    void prefetch(int keylenx) const {
	if (keylenx < 128)
	    prefetcher_type()(u_.v);
	else
	    u_.n->prefetch_full();
    }

  private:
    union {
	node_base<P>* n;
	value_type v;
	uintptr_t x;
    } u_;
};

template <typename P>
class leaf : public node_base<P> {
  public:
    static constexpr int width = P::leaf_width;
    typedef typename node_base<P>::nodeversion_type nodeversion_type;
    typedef key<typename P::ikey_type> key_type;
    typedef typename node_base<P>::leafvalue_type leafvalue_type;
    typedef kpermuter<P::leaf_width> permuter_type;
    typedef typename P::ikey_type ikey_type;
    typedef typename key_bound<width, P::bound_method>::type bound_type;
    typedef typename P::threadinfo_type threadinfo;

    int8_t extrasize64_;
    int8_t nremoved_;
    uint8_t keylenx_[width];
    typename permuter_type::storage_type permutation_;
    ikey_type ikey0_[width];
    leafvalue_type lv_[width];
    stringbag<uint32_t>* ksuf_;	// a real rockstar would save this space
				// when it is unsed
    union {
	leaf<P>* ptr;
	uintptr_t x;
    } next_;
    leaf<P>* prev_;
    node_base<P>* parent_;
    kvtimestamp_t node_ts_;
    kvtimestamp_t created_at_[P::debug_level > 0];
    stringbag<uint16_t> iksuf_[0];

    leaf(size_t sz, kvtimestamp_t node_ts)
	: node_base<P>(true), nremoved_(0),
	  permutation_(permuter_type::make_empty()),
	  ksuf_(), parent_(), node_ts_(node_ts), iksuf_{} {
	masstree_precondition(sz % 64 == 0 && sz / 64 < 128);
	extrasize64_ = (int(sz) >> 6) - ((int(sizeof(*this)) + 63) >> 6);
	if (extrasize64_ > 0)
	    new((void *)&iksuf_[0]) stringbag<uint16_t>(width, sz - sizeof(*this));
    }

    static leaf<P>* make(int ksufsize, kvtimestamp_t node_ts, threadinfo& ti) {
	size_t sz = iceil(sizeof(leaf<P>) + std::min(ksufsize, 128), 64);
	void* ptr = ti.pool_allocate(sz, memtag_masstree_leaf);
	leaf<P>* n = new(ptr) leaf<P>(sz, node_ts);
	assert(n);
	if (P::debug_level > 0)
	    n->created_at_[0] = ti.operation_timestamp();
	return n;
    }
    static leaf<P>* make_root(int ksufsize, leaf<P>* parent, threadinfo& ti) {
        leaf<P>* n = make(ksufsize, parent ? parent->node_ts_ : 0, ti);
        n->next_.ptr = n->prev_ = 0;
        n->parent_ = node_base<P>::parent_for_layer_root(parent);
        n->mark_root();
        return n;
    }

    size_t allocated_size() const {
	int es = (extrasize64_ >= 0 ? extrasize64_ : -extrasize64_ - 1);
	return (sizeof(*this) + es * 64 + 63) & ~size_t(63);
    }
    int size() const {
	return permuter_type::size(permutation_);
    }
    permuter_type permutation() const {
	return permuter_type(permutation_);
    }
    typename nodeversion_type::value_type full_version_value() const {
        static_assert(int(nodeversion_type::traits_type::top_stable_bits) >= int(permuter_type::size_bits), "not enough bits to add size to version");
        return (this->version_value() << permuter_type::size_bits) + size();
    }

    using node_base<P>::has_changed;
    bool has_changed(nodeversion_type oldv,
                     typename permuter_type::storage_type oldperm) const {
        return this->has_changed(oldv) || oldperm != permutation_;
    }

    key_type get_key(int p) const {
	int kl = keylenx_[p];
	if (!keylenx_has_ksuf(kl))
	    return key_type(ikey0_[p], kl);
	else
	    return key_type(ikey0_[p], ksuf(p));
    }
    ikey_type ikey(int p) const {
	return ikey0_[p];
    }
    ikey_type ikey_bound() const {
	return ikey0_[0];
    }
    int ikeylen(int p) const {
	return keylenx_ikeylen(keylenx_[p]);
    }
    inline int stable_last_key_compare(const key_type& k, nodeversion_type v,
                                       threadinfo& ti) const;

    bool value_is_layer(int p) const {
	return keylenx_is_layer(keylenx_[p]);
    }
    bool value_is_stable_layer(int p) const {
	return keylenx_is_stable_layer(keylenx_[p]);
    }
    bool has_ksuf(int p) const {
	return keylenx_has_ksuf(keylenx_[p]);
    }
    Str ksuf(int p) const {
	masstree_precondition(has_ksuf(p));
	return ksuf_ ? ksuf_->get(p) : iksuf_[0].get(p);
    }
    static int keylenx_ikeylen(int keylenx) {
	return keylenx & 63;
    }
    static bool keylenx_is_layer(int keylenx) {
	return keylenx > 63;
    }
    static bool keylenx_is_unstable_layer(int keylenx) {
	return keylenx & 64;
    }
    static bool keylenx_is_stable_layer(int keylenx) {
	return keylenx > 127;	// see also leafvalue
    }
    static bool keylenx_has_ksuf(int keylenx) {
	return keylenx == (int) sizeof(ikey_type) + 1;
    }
    size_t ksuf_size() const {
	if (extrasize64_ <= 0)
	    return ksuf_ ? ksuf_->size() : 0;
	else
	    return iksuf_[0].size();
    }
    bool ksuf_equals(int p, const key_type& ka) {
	// Precondition: keylenx_[p] == ka.ikeylen() && ikey0_[p] == ka.ikey()
	return ksuf_equals(p, ka, keylenx_[p]);
    }
    bool ksuf_equals(int p, const key_type& ka, int keylenx) {
	// Precondition: keylenx_[p] == ka.ikeylen() && ikey0_[p] == ka.ikey()
	return !keylenx_has_ksuf(keylenx)
	    || (!ksuf_ && iksuf_[0].equals_sloppy(p, ka.suffix()))
	    || (ksuf_ && ksuf_->equals_sloppy(p, ka.suffix()));
    }
    int ksuf_compare(int p, const key_type& ka) {
	if (!has_ksuf(p))
	    return 0;
	else if (!ksuf_)
	    return iksuf_[0].compare(p, ka.suffix());
	else
	    return ksuf_->compare(p, ka.suffix());
    }

    bool deleted_layer() const {
	return nremoved_ > width;
    }
    void mark_deleted_layer() {
	nremoved_ = width + 1;
    }

    void assign(int p, const key_type& ka, threadinfo& ti) {
	lv_[p] = leafvalue_type::make_empty();
	if (ka.has_suffix())
	    assign_ksuf(p, ka.suffix(), false, ti);
	ikey0_[p] = ka.ikey();
	keylenx_[p] = ka.ikeylen();
    }
    void assign_initialize(int p, const key_type& ka, threadinfo& ti) {
	lv_[p] = leafvalue_type::make_empty();
	if (ka.has_suffix())
	    assign_ksuf(p, ka.suffix(), true, ti);
	ikey0_[p] = ka.ikey();
	keylenx_[p] = ka.ikeylen();
    }
    void assign_initialize(int p, leaf<P>* x, int xp, threadinfo& ti) {
	lv_[p] = x->lv_[xp];
	if (x->has_ksuf(xp))
	    assign_ksuf(p, x->ksuf(xp), true, ti);
	ikey0_[p] = x->ikey0_[xp];
	keylenx_[p] = x->keylenx_[xp];
    }
    void assign_ksuf(int p, Str s, bool initializing, threadinfo& ti) {
	if (extrasize64_ <= 0 || !iksuf_[0].assign(p, s))
	    hard_assign_ksuf(p, s, initializing, ti);
    }
    void hard_assign_ksuf(int p, Str s, bool initializing, threadinfo& ti);

    void prefetch() const {
	for (int i = 64; i < std::min(16 * width + 1, 4 * 64); i += 64)
	    ::prefetch((const char *) this + i);
	if (extrasize64_ > 0)
	    ::prefetch((const char *) &iksuf_[0]);
	else if (extrasize64_ < 0) {
	    ::prefetch((const char *) ksuf_);
	    ::prefetch((const char *) ksuf_ + CACHE_LINE_SIZE);
	}
    }

    void print(FILE* f, const char* prefix, int indent, int kdepth);

    leaf<P>* safe_next() const {
	return reinterpret_cast<leaf<P>*>(next_.x & ~(uintptr_t) 1);
    }

    void deallocate(threadinfo& ti) {
	if (ksuf_)
	    ti.deallocate(ksuf_, ksuf_->allocated_size(),
                              memtag_masstree_ksuffixes);
	ti.pool_deallocate(this, allocated_size(), memtag_masstree_leaf);
    }
    void deallocate_rcu(threadinfo& ti) {
	if (ksuf_)
	    ti.deallocate_rcu(ksuf_, ksuf_->allocated_size(),
                              memtag_masstree_ksuffixes);
	ti.pool_deallocate_rcu(this, allocated_size(), memtag_masstree_leaf);
    }
};


template <typename P>
void basic_table<P>::initialize(threadinfo& ti) {
    masstree_precondition(!root_);
    reinitialize(ti);
}

template <typename P>
void basic_table<P>::reinitialize(threadinfo& ti) {
    root_ = node_type::leaf_type::make_root(0, 0, ti);
}

/** @brief Assign position @a p's keysuffix to @a s.

    This version of assign_ksuf() is called when @a s might not fit into
    the current keysuffix container. It may allocate a new container, copying
    suffixes over.

    The @a initializing parameter determines which suffixes are copied. If @a
    initializing is false, then this is an insertion into a live node. The
    live node's permutation indicates which keysuffixes are active, and only
    active suffixes are copied. If @a initializing is true, then this
    assignment is part of the initialization process for a new node. The
    permutation might not be set up yet. In this case, it is assumed that key
    positions [0,p) are ready: keysuffixes in that range are copied. In either
    case, the key at position p is NOT copied; it is assigned to @a s. */
template <typename P>
void leaf<P>::hard_assign_ksuf(int p, Str s, bool initializing,
			       threadinfo& ti) {
    if (ksuf_ && ksuf_->assign(p, s))
	return;

    stringbag<uint16_t> *iksuf;
    stringbag<uint32_t> *oksuf;
    if (extrasize64_ > 0)
	iksuf = &iksuf_[0], oksuf = 0;
    else
	iksuf = 0, oksuf = ksuf_;

    size_t csz;
    if (iksuf)
	csz = iksuf->allocated_size() - iksuf->overhead(width);
    else if (oksuf)
	csz = oksuf->allocated_size() - oksuf->overhead(width);
    else
	csz = 0;
    size_t sz = iceil_log2(std::max(csz, size_t(4 * width)) * 2);
    while (sz < csz + stringbag<uint32_t>::overhead(width) + s.len)
	sz *= 2;

    void *ptr = ti.allocate(sz, memtag_masstree_ksuffixes);
    stringbag<uint32_t> *nksuf = new(ptr) stringbag<uint32_t>(width, sz);
    permuter_type perm(permutation_);
    int n = initializing ? p : perm.size();
    for (int i = 0; i < n; ++i) {
	int mp = initializing ? i : perm[i];
	if (mp != p && has_ksuf(mp)) {
	    bool ok = nksuf->assign(mp, ksuf(mp));
            assert(ok); (void) ok;
        }
    }
    bool ok = nksuf->assign(p, s);
    assert(ok); (void) ok;
    fence();

    if (nremoved_ > 0)		// removed ksufs are not copied to the new ksuf,
	this->mark_insert();	// but observers might need removed ksufs:
				// make them retry

    ksuf_ = nksuf;
    fence();

    if (extrasize64_ >= 0)	// now the new ksuf_ installed, mark old dead
	extrasize64_ = -extrasize64_ - 1;

    if (oksuf)
	ti.deallocate_rcu(oksuf, oksuf->allocated_size(),
                          memtag_masstree_ksuffixes);
}

} // namespace Masstree
#endif
