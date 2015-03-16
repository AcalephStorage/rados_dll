// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_TEST_RADOS_CLIENT_H
#define CEPH_TEST_RADOS_CLIENT_H

#include "include/rados/librados.hpp"
#include "common/config.h"
#include "include/atomic.h"
#include "include/buffer.h"
#include "test/librados_test_stub/TestWatchNotify.h"
#include <boost/function.hpp>
#include <boost/functional/hash.hpp>
#include <list>
#include <map>
#include <string>
#include <vector>

class Finisher;

namespace librados {

class TestIoCtxImpl;

class TestRadosClient {
public:

  typedef boost::function<int()> AioFunction;

  struct Object {
    std::string oid;
    std::string locator;
    std::string nspace;
  };

  TestRadosClient(CephContext *cct);

  void get();
  void put();

  virtual CephContext *cct();

  virtual uint64_t get_instance_id();

  virtual int connect();
  virtual void shutdown();
  virtual int wait_for_latest_osdmap();

  virtual TestIoCtxImpl *create_ioctx(int64_t pool_id,
                                      const std::string &pool_name) = 0;

  virtual int mon_command(const std::vector<std::string>& cmd,
                          const bufferlist &inbl,
                          bufferlist *outbl, std::string *outs);

  virtual void object_list(int64_t pool_id,
			   std::list<librados::TestRadosClient::Object> *list) = 0;

  virtual int pool_create(const std::string &pool_name) = 0;
  virtual int pool_delete(const std::string &pool_name) = 0;
  virtual int pool_get_base_tier(int64_t pool_id, int64_t* base_tier) = 0;
  virtual int pool_list(std::list<std::pair<int64_t, std::string> >& v) = 0;
  virtual int64_t pool_lookup(const std::string &name) = 0;
  virtual int pool_reverse_lookup(int64_t id, std::string *name) = 0;

  virtual int watch_flush() = 0;

  virtual int blacklist_add(const std::string& client_address,
			    uint32_t expire_seconds) = 0;

  TestWatchNotify &get_watch_notify() {
    return m_watch_notify;
  }

  void add_aio_operation(const std::string& oid,
			 const AioFunction &aio_function, AioCompletionImpl *c);
  void flush_aio_operations();
  void flush_aio_operations(AioCompletionImpl *c);

protected:
  virtual ~TestRadosClient();

private:

  CephContext *m_cct;
  atomic_t m_refcount;

  Finisher *get_finisher(const std::string& oid);

  std::vector<Finisher *> m_finishers;
  boost::hash<std::string> m_hash;

  TestWatchNotify m_watch_notify;

};

} // namespace librados

#endif // CEPH_TEST_RADOS_CLIENT_H
