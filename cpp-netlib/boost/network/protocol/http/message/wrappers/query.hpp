#ifndef BOOST_NETWORK_PROTOCOL_HTTP_MESSAGE_WRAPPERS_QUERY_HPP_20100618
#define BOOST_NETWORK_PROTOCOL_HTTP_MESSAGE_WRAPPERS_QUERY_HPP_20100618

// Copyright 2010 (c) Dean Michael Berris.
// Copyright 2010 (c) Sinefunc, Inc.
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

namespace boost {
namespace network {
namespace http {

template <class Tag>
struct basic_request;

namespace impl {

template <class Tag>
struct query_wrapper {
  basic_request<Tag> const& message_;

  query_wrapper(basic_request<Tag> const& message) : message_(message) {}

  typedef typename basic_request<Tag>::string_type string_type;

  operator string_type() { return message_.query(); }
};

}  // namespace impl

template <class Tag>
inline impl::query_wrapper<Tag> query(basic_request<Tag> const& request) {
  return impl::query_wrapper<Tag>(request);
}

}  // namespace http

}  // namespace network

}  // namespace boost

#endif  // BOOST_NETWORK_PROTOCOL_HTTP_MESSAGE_WRAPPERS_QUERY_HPP_20100618
