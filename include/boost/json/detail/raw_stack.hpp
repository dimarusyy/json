//
// Copyright (c) 2018-2019 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/vinniefalco/json
//

#ifndef BOOST_JSON_DETAIL_RAW_STACK_HPP
#define BOOST_JSON_DETAIL_RAW_STACK_HPP

#include <boost/json/detail/config.hpp>
#include <boost/json/storage_ptr.hpp>
#include <boost/json/detail/assert.hpp>
#include <cstdlib>
#include <utility>

namespace boost {
namespace json {
namespace detail {

class raw_stack
{
    storage_ptr sp_;
    std::size_t size_ = 0;
    std::size_t capacity_ = 0;
    char* base_ = nullptr;

public:
    explicit
    raw_stack(
        storage_ptr sp = {})
        : sp_(std::move(sp))
    {
    }

    ~raw_stack()
    {
        if(base_)
            sp_->deallocate(
                base_, size_);
    }

    void*
    push(std::size_t n)
    {
        if(n > capacity_ - size_)
            grow(n - (
                capacity_ - size_));
        auto p = base_ + size_;
        size_ += n;
        return p;
    }

    void*
    pop(std::size_t n) noexcept
    {
        BOOST_JSON_ASSERT(
            n <= size_);
        size_ -= n;
        return base_ + size_;
    }

    void
    add(std::size_t n)
    {
        if(n > capacity_ - size_)
            grow(n - (
                capacity_ - size_));
        size_ += n;
    }

    void
    subtract(std::size_t n)
    {
        BOOST_JSON_ASSERT(
            n <= size_);
        size_ -= n;
    }

private:
    static
    constexpr
    std::size_t
    min_capacity_ = 1024;

    static
    constexpr
    std::size_t
    max_size() noexcept
    {
        return std::size_t(-1);
    }

    BOOST_JSON_DECL
    void
    grow(std::size_t n);
};

} // detail
} // json
} // boost

#ifdef BOOST_JSON_HEADER_ONLY
#include <boost/json/detail/raw_stack.ipp>
#endif

#endif