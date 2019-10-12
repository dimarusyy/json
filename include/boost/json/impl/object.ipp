//
// Copyright (c) 2018-2019 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/vinniefalco/json
//

#ifndef BOOST_JSON_IMPL_OBJECT_IPP
#define BOOST_JSON_IMPL_OBJECT_IPP

#include <boost/json/object.hpp>
#include <boost/core/exchange.hpp>
#include <boost/core/ignore_unused.hpp>
#include <boost/throw_exception.hpp>
#include <boost/assert.hpp>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace boost {
namespace json {

//------------------------------------------------------------------------------

string_view
object::
element::
key() const noexcept
{
    auto p = reinterpret_cast<
        char const*>(this + 1);
    auto const result =
        detail::varint_read(p);
    return {
        p + result.second,
        static_cast<std::size_t>(
            result.first) };
}

void
object::
element::
destroy(
    element const* e,
    storage_ptr const& sp) noexcept
{
    auto const len = e->key().size();
    auto const n =
        detail::varint_size(len);
    e->~element();
    sp->deallocate(
        const_cast<element*>(e),
        sizeof(element) + n + len + 1,
        alignof(element));
}

//------------------------------------------------------------------------------

struct object::table
{
    // number of values in the object
    std::size_t size = 0;

    // number of buckets in table
    std::size_t bucket_count = 0;

    // insertion-order list of all objects
    element* head;

    list_hook end_element;

    table() noexcept
        : head(end())
    {
        head->next = nullptr;
        head->prev = nullptr;
    }

    element*
    begin() noexcept
    {
        return head;
    }

    element*
    end() noexcept
    {
        return reinterpret_cast<
            element*>(&end_element);
    }

    element*&
    bucket(std::size_t n) noexcept
    {
        return reinterpret_cast<
            element**>(this + 1)[n];
    }

    static
    table*
    construct(
        size_type bucket_count,
        storage_ptr const& sp)
    {
        auto tab = ::new(sp->allocate(
            sizeof(table) +
                bucket_count *
                sizeof(element*),
            (std::max)(
                alignof(table),
                alignof(element*))
                    )) table;
        tab->size = 0;
        tab->bucket_count = bucket_count;
        auto it = &tab->bucket(0);
        auto const last =
            &tab->bucket(bucket_count);
        while(it != last)
            *it++ = tab->end();
        return tab;
    }

    static
    void
    destroy(
        table* tab,
        storage_ptr const& sp) noexcept
    {
        auto const n =
            tab->bucket_count;
        tab->~table();
        sp->deallocate(
            tab,
            sizeof(table) +
                n * sizeof(element*),
            alignof(table));
    }
};

//------------------------------------------------------------------------------

object::
undo_range::
undo_range(object& self) noexcept
    : self_(self)
{
}

void
object::
undo_range::
insert(element* e) noexcept
{
    if(! head_)
    {
        head_ = e;
        tail_ = e;
        e->prev = nullptr;
    }
    else
    {
        e->prev = tail_;
        tail_->next = e;
        tail_ = e;
    }
    e->next = nullptr;
    ++n_;
}

object::
undo_range::
~undo_range()
{
    for(auto it = head_; it;)
    {
        auto e = it;
        it = it->next;
        element::destroy(
            e, self_.sp_);
    }
}

void
object::
undo_range::
commit(
    const_iterator pos,
    size_type min_buckets)
{
    if(head_ == nullptr)
        return;
    auto& tab = self_.tab_;
    auto before = pos.e_;

    // add space for n_ elements.
    //
    // this is the last allocation, so
    // we never have to clean it up on
    // an exception.
    //
    bool const at_end =
        before == nullptr ||
        before == tab->end();
    self_.rehash((std::max)(min_buckets,
        static_cast<size_type>(std::ceil(
            (self_.size() + n_) /
                self_.max_load_factor()))));
    // refresh `before`, which
    // may have been invalidated
    if(at_end)
        before = tab->end();

    // insert each item into buckets
    for(auto it = head_; it;)
    {
        auto const e = it;
        it = it->next;
        // discard dupes
        auto const result =
            self_.find_impl(e->key());
        if(result.first)
        {
            element::destroy(e, self_.sp_);
            continue;
        }
        // add to list
        e->next = before;
        e->prev = before->prev;
        before->prev = e;
        if(e->prev)
            e->prev->next = e;
        else
            tab->head = e;
        // add to bucket
        auto const bn = constrain_hash(
            result.second, tab->bucket_count);
        auto& local_head = tab->bucket(bn);
        e->local_next = local_head;
        local_head = e;
        ++tab->size;
    }
    // do nothing in dtor
    head_ = nullptr;
}

//------------------------------------------------------------------------------

std::pair<
    std::uint64_t,
    std::uint64_t>
object::
hasher::
init(std::true_type) noexcept
{
    return {
        0x100000001B3ULL,
        0xcbf29ce484222325ULL
    };
}

std::pair<
    std::uint32_t,
    std::uint32_t>
object::
hasher::
init(std::false_type) noexcept
{
    return {
        0x01000193UL,
        0x811C9DC5UL
    };
}

std::size_t
object::
hasher::
operator()(key_type key) const noexcept
{
    std::size_t prime;
    std::size_t hash;
    std::tie(prime, hash) = init(
        std::integral_constant<bool,
            sizeof(std::size_t) >=
        sizeof(unsigned long long)>{});
    for(auto p = key.begin(),
        end = key.end(); p != end; ++p)
        hash = (*p ^ hash) * prime;
    return hash;
}

//------------------------------------------------------------------------------

object::
node_type::
node_type(
    element* e,
    storage_ptr sp) noexcept
    : e_(e)
    , sp_(std::move(sp))
{
}

object::
node_type::
~node_type()
{
    if(e_)
        element::destroy(e_, sp_);
}

object::
node_type::
node_type(
    node_type&& other) noexcept
    : e_(boost::exchange(
        other.e_, nullptr))
    , sp_(boost::exchange(
        other.sp_, nullptr))
{
}

auto
object::
node_type::
operator=(node_type&& other) noexcept ->
    node_type&
{
    node_type temp;
    temp.swap(*this);
    this->swap(other);
    return *this;
}

void
swap(
    object::node_type& lhs,
    object::node_type& rhs) noexcept
{
    lhs.swap(rhs);
}

//------------------------------------------------------------------------------
//
// Iterators
//
//------------------------------------------------------------------------------

object::
const_iterator::
const_iterator(
    iterator it) noexcept
    : e_(it.e_)
{
}

object::
const_iterator::
const_iterator(
    local_iterator it) noexcept
    : e_(it.e_)
{
}

object::
const_iterator::
const_iterator(
    const_local_iterator it) noexcept
    : e_(it.e_)
{
}

object::
iterator::
iterator(
    local_iterator it) noexcept
    : e_(it.e_)
{
}

object::
const_local_iterator::
const_local_iterator(
    local_iterator it) noexcept
    : e_(it.e_)
{
}

//------------------------------------------------------------------------------
//
// Special Members
//
//------------------------------------------------------------------------------

object::
~object()
{
    if(! tab_)
        return;
    for(auto it = tab_->head;
        it != tab_->end();)
    {
        auto next = it->next;
        element::destroy(it, sp_);
        it = next;
    }
    table::destroy(tab_, sp_);
}

object::
object() noexcept
    : sp_(default_storage())
{
}

object::
object(storage_ptr sp) noexcept
    : sp_(std::move(sp))
{
}

object::
object(
    size_type bucket_count)
    : object(
        bucket_count,
        default_storage())
{
}

object::
object(
    size_type bucket_count,
    storage_ptr sp)
    : sp_(std::move(sp))
{
    rehash(bucket_count);
}

object::
object(object&& other) noexcept
    : sp_(other.sp_)
    , tab_(boost::exchange(
        other.tab_, nullptr))
    , mf_(other.mf_)
{
}

object::
object(
    object&& other,
    storage_ptr sp)
    : sp_(std::move(sp))
    , mf_(other.mf_)
{
    if(*sp_ == *other.sp_)
    {
        tab_ = boost::exchange(
            other.tab_, nullptr);
    }
    else
    {
        insert_range(
            end(),
            other.begin(),
            other.end(),
            0);
    }
}

object::
object(pilfered<object> other) noexcept
    : sp_(std::move(other.get().sp_))
    , tab_(boost::exchange(
        other.get().tab_, nullptr))
    , mf_(other.get().mf_)
{
}

object::
object(
    object const& other)
    : object(
        other,
        other.get_storage())
{
}

object::
object(
    object const& other,
    storage_ptr sp)
    : sp_(std::move(sp))
    , mf_(other.mf_)
{
    insert_range(
        end(),
        other.begin(),
        other.end(),
        0);
}

object::
object(
    std::initializer_list<
        init_value> init)
    : object(
        init,
        init.size(),
        default_storage())
{
}

object::
object(
    std::initializer_list<
        init_value> init,
    size_type bucket_count)
    : object(
        init,
        bucket_count,
        default_storage())
{
}

object::
object(
    std::initializer_list<
        init_value> init,
    storage_ptr sp)
    : object(
        init,
        init.size(),
        std::move(sp))
{
}      
        
object::
object(
    std::initializer_list<
        init_value> init,
    size_type bucket_count,
    storage_ptr sp)
    : sp_(std::move(sp))
{
    insert_range(
        end(),
        init.begin(),
        init.end(),
        bucket_count);
}

object&
object::
operator=(object&& other)
{
    object tmp(std::move(other), sp_);
    this->~object();
    ::new(this) object(pilfer(tmp));
    return *this;
}

object&
object::
operator=(object const& other)
{
    object tmp(other, sp_);
    this->~object();
    ::new(this) object(pilfer(tmp));
    return *this;
}

object&
object::
operator=(
    std::initializer_list<
        init_value> init)
{
    object tmp(init, sp_);
    this->~object();
    ::new(this) object(pilfer(tmp));
    return *this;
}

storage_ptr const&
object::
get_storage() const noexcept
{
    return sp_;
}

//------------------------------------------------------------------------------
//
// Iterators
//
//------------------------------------------------------------------------------

auto
object::
begin() noexcept ->
    iterator
{
    if(! tab_)
        return {};
    return tab_->head;
}

auto
object::
begin() const noexcept ->
    const_iterator
{
    if(! tab_)
        return {};
    return tab_->head;
}

auto
object::
cbegin() const noexcept ->
    const_iterator
{
    return begin();
}

auto
object::
end() noexcept ->
    iterator
{
    if(! tab_)
        return {};
    return tab_->end();
}

auto
object::
end() const noexcept ->
    const_iterator
{
    if(! tab_)
        return {};
    return tab_->end();
}

auto
object::
cend() const noexcept ->
    const_iterator
{
    return end();
}

#if 0
auto
object::
rbegin() noexcept ->
    reverse_iterator
{
    return reverse_iterator(end());
}

auto
object::
rbegin() const noexcept ->
    const_reverse_iterator
{
    return const_reverse_iterator(end());
}

auto
object::
crbegin() const noexcept ->
    const_reverse_iterator
{
    return rbegin();
}

auto
object::
rend() noexcept ->
    reverse_iterator
{
    return reverse_iterator(begin());
}

auto
object::
rend() const noexcept ->
    const_reverse_iterator
{
    return const_reverse_iterator(begin());
}

auto
object::
crend() const noexcept ->
    const_reverse_iterator
{
    return rend();
}
#endif

//------------------------------------------------------------------------------
//
// Capacity
//
//------------------------------------------------------------------------------

bool
object::
empty() const noexcept
{
    return ! tab_ ||
        tab_->size == 0;
}

auto
object::
size() const noexcept ->
    size_type
{
    if(! tab_)
        return 0;
    return tab_->size;
}

auto
object::
max_size() const noexcept ->
    size_type
{
    return (std::numeric_limits<
        size_type>::max)();
}

//------------------------------------------------------------------------------
//
// Modifiers
//
//------------------------------------------------------------------------------

void
object::
clear() noexcept
{
    object tmp(std::move(*this));
}

void
object::
insert(
    std::initializer_list<
        init_value> init)
{
    insert_range(
        end(),
        init.begin(),
        init.end(),
        0);
}

void
object::
insert(
    const_iterator pos,
    std::initializer_list<
        init_value> init)
{
    insert_range(
        pos,
        init.begin(),
        init.end(),
        0);
}

auto
object::
insert(node_type&& nh) ->
    insert_return_type
{
    return insert(end(), std::move(nh));
}

auto
object::
insert(
    const_iterator pos,
    node_type&& nh) ->
        insert_return_type
{
    if(! nh.e_)
        return { end(), {}, false };
    BOOST_ASSERT(
        *nh.get_storage() == *sp_);
    auto const result =
        find_impl(nh.key());
    if(result.first)
        return { iterator(result.first),
            std::move(nh), false };
    insert(pos, result.second, nh.e_);
    return { iterator(boost::exchange(
        nh.e_, nullptr)), {}, true };
}

auto
object::
erase(const_iterator pos) ->
    iterator
{
    auto e = pos.e_;
    pos = e->next;
    remove(e);
    element::destroy(e, sp_);
    return iterator(pos.e_);
}

auto
object::
erase(
    const_iterator first,
    const_iterator last) ->
        iterator
{
    while(first != last)
    {
        auto e = first.e_;
        first = e->next;
        remove(e);
        element::destroy(e, sp_);
    }
    return iterator(first.e_);
}

auto
object::
erase(key_type key) ->
    size_type
{
    auto it = find(key);
    if(it == end())
        return 0;
    erase(it);
    return 1;
}

void
object::
swap(object& other)
{
    if(*sp_ == *other.sp_)
    {
        std::swap(tab_, other.tab_);
        std::swap(mf_, other.mf_);
        return;
    }

    object temp1(
        std::move(*this),
        other.get_storage());
    object temp2(
        std::move(other),
        this->get_storage());
    other.~object();
    ::new(&other) object(pilfer(temp1));
    this->~object();
    ::new(this) object(pilfer(temp2));
}

auto
object::
extract(const_iterator pos) ->
    node_type
{
    BOOST_ASSERT(pos != end());
    remove(pos.e_);
    return { pos.e_, sp_ };
}

auto
object::
extract(key_type key) ->
    node_type
{
    auto it = find(key);
    if(it == end())
        return {};
    return extract(it);
}

//------------------------------------------------------------------------------
//
// Lookup
//
//------------------------------------------------------------------------------

auto
object::
at(key_type key) ->
    value&
{
    auto it = find(key);
    if(it == end())
        BOOST_THROW_EXCEPTION(
         std::out_of_range(
            "key not found"));
    return it->second;
}
    
auto
object::
at(key_type key) const ->
    value const&
{
    auto it = find(key);
    if(it == end())
        BOOST_THROW_EXCEPTION(
         std::out_of_range(
            "key not found"));
    return it->second;
}

auto
object::
operator[](key_type key) ->
    value&
{
    auto const result = emplace(
        end(), key, kind::null);
    return result.first->second;
}

auto
object::
count(key_type key) const noexcept ->
    size_type
{
    if(find(key) == end())
        return 0;
    return 1;
}

auto
object::
find(key_type key) noexcept ->
    iterator
{
    auto const e =
        find_impl(key).first;
    if(e)
        return {e};
    return end();
}

auto
object::
find(key_type key) const noexcept ->
    const_iterator
{
    auto const e =
        find_impl(key).first;
    if(e)
        return {e};
    return end();
}

bool
object::
contains(key_type key) const noexcept
{
    return find(key) != end();
}

//------------------------------------------------------------------------------
//
// Bucket Interface
//
//------------------------------------------------------------------------------

namespace detail {

struct primes
{
    using value_type = std::size_t;
    using iterator = std::size_t const*;

    std::size_t const* begin_;
    std::size_t const* end_;

    iterator
    begin() const noexcept
    {
        return begin_;
    }

    iterator
    end() const noexcept
    {
        return end_;
    }
};

// Taken from Boost.Intrusive and Boost.MultiIndex code,
// thanks to Ion Gaztanaga and Joaquin M Lopez Munoz.

template<class = void>
primes
get_primes(std::false_type) noexcept
{
    static std::size_t constexpr list[] = {
        0UL,

        3UL,                     7UL,
        11UL,                    17UL,
        29UL,                    53UL,
        97UL,                    193UL,
        389UL,                   769UL,
        1543UL,                  3079UL,
        6151UL,                  12289UL,
        24593UL,                 49157UL,
        98317UL,                 196613UL,
        393241UL,                786433UL,
        1572869UL,               3145739UL,
        6291469UL,               12582917UL,
        25165843UL,              50331653UL,
        100663319UL,             201326611UL,
        402653189UL,             805306457UL,
        1610612741UL,            3221225473UL,

        4294967291UL,            4294967295UL
    };
    return {
        &list[0],
        &list[std::extent<
            decltype(list)>::value] };
}

template<class = void>
primes
get_primes(std::true_type) noexcept
{
    static std::size_t constexpr list[] = {
        0ULL,

        3ULL,                     7ULL,
        11ULL,                    17ULL,
        29ULL,                    53ULL,
        97ULL,                    193ULL,
        389ULL,                   769ULL,
        1543ULL,                  3079ULL,
        6151ULL,                  12289ULL,
        24593ULL,                 49157ULL,
        98317ULL,                 196613ULL,
        393241ULL,                786433ULL,
        1572869ULL,               3145739ULL,
        6291469ULL,               12582917ULL,
        25165843ULL,              50331653ULL,
        100663319ULL,             201326611ULL,
        402653189ULL,             805306457ULL,
        1610612741ULL,            3221225473ULL,

        6442450939ULL,            12884901893ULL,
        25769803751ULL,           51539607551ULL,
        103079215111ULL,          206158430209ULL,
        412316860441ULL,          824633720831ULL,
        1649267441651ULL,         3298534883309ULL,
        6597069766657ULL,         13194139533299ULL,
        26388279066623ULL,        52776558133303ULL,
        105553116266489ULL,       211106232532969ULL,
        422212465066001ULL,       844424930131963ULL,
        1688849860263953ULL,      3377699720527861ULL,
        6755399441055731ULL,      13510798882111483ULL,
        27021597764222939ULL,     54043195528445957ULL,
        108086391056891903ULL,    216172782113783843ULL,
        432345564227567621ULL,    864691128455135207ULL,
        1729382256910270481ULL,   3458764513820540933ULL,
        6917529027641081903ULL,   13835058055282163729ULL,
        18446744073709551557ULL,  18446744073709551615ULL
    };
    return {
        &list[0],
        &list[std::extent<
            decltype(list)>::value] };
}

BOOST_JSON_DECL
primes
get_primes() noexcept
{
    return get_primes(
        std::integral_constant<bool,
            sizeof(std::size_t) >=
                sizeof(unsigned long long)>{});
}

} // detail

//------------------------------------------------------------------------------

auto
object::
begin(size_type n) ->
    local_iterator
{
    BOOST_ASSERT(tab_);
    return tab_->bucket(n);
}

auto
object::
begin(size_type n) const ->
    const_local_iterator
{
    BOOST_ASSERT(tab_);
    return tab_->bucket(n);
}

auto
object::
cbegin(size_type n) const ->
    const_local_iterator
{
    BOOST_ASSERT(tab_);
    return tab_->bucket(n);
}

auto
object::
end(size_type n)  ->
    local_iterator
{
    boost::ignore_unused(n);
    BOOST_ASSERT(tab_);
    return tab_->end();
}

auto
object::
end(size_type n) const ->
    const_local_iterator
{
    boost::ignore_unused(n);
    BOOST_ASSERT(tab_);
    return tab_->end();
}

auto
object::
cend(size_type n) const ->
    const_local_iterator
{
    boost::ignore_unused(n);
    BOOST_ASSERT(tab_);
    return tab_->end();
}

auto
object::
bucket_count() const noexcept ->
    size_type
{
    if(! tab_)
        return 0;
    return tab_->bucket_count;
}

auto
object::
max_bucket_count() const noexcept ->
    size_type
{
    return detail::get_primes().end()[-1];
}

auto
object::
bucket_size(size_type n) const ->
    size_type
{
    BOOST_ASSERT(tab_);
    size_type size = 0;
    for(auto e = tab_->bucket(n);
        e != tab_->end();
        e = e->local_next)
        ++size;
    return size;
}

auto
object::
bucket(key_type key) const ->
    size_type
{
    BOOST_ASSERT(tab_);
    return constrain_hash(
        hash_function()(key),
        bucket_count());
}

//------------------------------------------------------------------------------
//
// Hash Policy
//
//------------------------------------------------------------------------------

float
object::
load_factor() const noexcept
{
    if(! tab_)
        return 0;
    return static_cast<float>(
        size()) / bucket_count();
}

void
object::
max_load_factor(float mf)
{
    mf_ = mf;
    if(load_factor() > mf_)
        rehash(0);
}

// rehash to at least `n` buckets
void
object::
rehash(size_type n)
{
    // snap to nearest prime 
    auto const primes =
        detail::get_primes();
    n = *std::lower_bound(
        primes.begin(), primes.end(), n);
    auto const bc = bucket_count();
    if(n == bc)
        return;
    if(n < bc)
    {
        n = (std::max<size_type>)(
            n, *std::lower_bound(
            primes.begin(), primes.end(),
            static_cast<size_type>(
                std::ceil(size() /
                max_load_factor()))));
        if(n <= bc)
            return;
    }
    // create new buckets
    auto tab = table::construct(n, sp_);
    if(tab_)
    {
        tab->size = tab_->size;
        if(tab_->head != tab_->end())
        {
            tab->head = tab_->head;
            tab->end()->prev =
                tab_->end()->prev;
            tab->end()->prev->next =
                tab->end();
        }
        else
        {
            tab->head = tab->end();
        }
        table::destroy(tab_, sp_);
    }
    tab_ = tab;
    // rehash into new buckets
    for(auto e = tab_->head;
        e != tab_->end(); e = e->next)
    {
        auto const bn = bucket(e->key());
        auto& head = tab_->bucket(bn);
        e->local_next = head;
        head = e;
    }
}

void
object::
reserve(size_type n)
{
    rehash(static_cast<
        size_type>(std::ceil(
            n / max_load_factor())));
}

//------------------------------------------------------------------------------

auto
object::
hash_function() const noexcept ->
    hasher
{
    return hasher{};
}

auto
object::
key_eq() const noexcept ->
    key_equal
{
    return key_equal{};
}

//------------------------------------------------------------------------------

// allocate a new element
auto
object::
allocate_impl(
    key_type key,
    construct_base const& place_new) ->
        element*
{
    auto const n = static_cast<std::size_t>(
        detail::varint_size(key.size()));
    auto const size =
        sizeof(element) + n + key.size() + 1;
    struct cleanup
    {
        void* p;
        std::size_t size;
        storage_ptr const& sp;

        ~cleanup()
        {
            if(p)
                sp->deallocate(p, size,
                    alignof(element));
        }
    };
    cleanup c{sp_->allocate(
        size, alignof(element)), size, sp_};
    place_new(c.p);
    char* p = static_cast<char*>(c.p);
    c.p = nullptr;
    detail::varint_write(
        p + sizeof(element), key.size());
    std::memcpy(
        p + sizeof(element) + n,
        key.data(),
        key.size());
    p[sizeof(element) +
        n + key.size()] = '\0';
    return reinterpret_cast<element*>(p);
}

auto
object::
allocate(std::pair<
    string_view, value const&> const& p) ->
        element*
{
    return allocate(p.first, p.second);
}

auto
object::
constrain_hash(
    std::size_t hash,
    size_type bucket_count) noexcept ->
        size_type
{
    return hash % bucket_count;
}

auto
object::
find_impl(key_type key) const noexcept ->
    std::pair<element*, std::size_t>
{
    auto const hash = hash_function()(key);
    auto bc = bucket_count();
    if(bc == 0)
        return { nullptr, hash };
    auto e = tab_->bucket(
        constrain_hash(hash, bc));
    auto const eq = key_eq();
    while(e != tab_->end())
    {
        if(eq(key, e->key()))
            return { e, hash };
        e = e->local_next;
    }
    return { nullptr, hash };
};

// destroys `e` on exception
void
object::
insert(
    const_iterator before,
    std::size_t hash,
    element* e)
{
    struct revert
    {
        element* e;
        storage_ptr const& sp;

        ~revert()
        {
            if(e)
                element::destroy(e, sp);
        }
    };
    revert r{e, sp_};

    // rehash if necessary
    if( size() + 1 >
        bucket_count() * max_load_factor())
    {
        auto const at_end = before == end();
        rehash(static_cast<size_type>(
            (std::ceil)(
                float(size()+1) /
                max_load_factor())));
        if(at_end)
            before = end();
    }

    // perform insert
    auto const bn = constrain_hash(
        hash, tab_->bucket_count);
    auto& head = tab_->bucket(bn);
    e->local_next = head;
    head = e;
    if(tab_->head == tab_->end())
    {
        BOOST_ASSERT(
            before.e_ == tab_->end());
        tab_->head = e;
        tab_->end()->prev = e;
        e->next = tab_->end();
        e->prev = nullptr;
    }
    else
    {
        e->prev = before.e_->prev;
        if(e->prev)
            e->prev->next = e;
        else
            tab_->head = e;
        e->next = before.e_;
        e->next->prev = e;
    }
    ++tab_->size;
    r.e = nullptr;
}

void
object::
remove(element* e)
{
    if(e == tab_->head)
    {
        tab_->head = e->next;
    }
    else
    {
        e->prev->next = e->next;
        e->next->prev = e->prev;
    }
    auto& head = tab_->bucket(
        bucket(e->key()));
    if(head != e)
    {
        auto it = head;
        BOOST_ASSERT(it != tab_->end());
        while(it->local_next != e)
        {
            it = it->local_next;
            BOOST_ASSERT(it != tab_->end());
        }
        it->local_next = e->local_next;
    }
    else
    {
        head = head->local_next;
    }
    --tab_->size;
}

void
swap(object& lhs, object& rhs)
{
    lhs.swap(rhs);
}

} // json
} // boost

#endif
