#ifndef CEPH_UNORDERED_MAP_H
#define CEPH_UNORDERED_MAP_H

#include <ciso646>

#ifdef _LIBCPP_VERSION

#include <unordered_map>
#ifdef _WIN32
namespace ceph {
  using std::unordered_map;
}
#else
namespace ceph {
  using std::unordered_map;
  using std::unordered_multimap;
}
#endif
#else

#include <tr1/unordered_map>
#ifdef _WIN32
namespace ceph {
  using std::tr1::unordered_map;
}
#else
namespace ceph {
  using std::tr1::unordered_map;
  using std::tr1::unordered_multimap;
}
#endif
#endif

#endif
