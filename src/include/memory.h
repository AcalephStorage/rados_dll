#ifndef CEPH_MEMORY_H
#define CEPH_MEMORY_H

#include <ciso646>

#ifdef _LIBCPP_VERSION

#include <memory>

namespace ceph {
  using std::shared_ptr;
  using std::weak_ptr;
  using std::static_pointer_cast;
}

#else

#include <tr1/memory>

namespace ceph {
  using std::tr1::shared_ptr;
  using std::tr1::weak_ptr;
  using std::tr1::static_pointer_cast;
}

#endif

#endif
