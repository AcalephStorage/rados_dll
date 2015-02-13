// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_TEST_IO_CTX_IMPL_H
#define CEPH_TEST_IO_CTX_IMPL_H

#include "include/rados/librados.hpp"
#include "include/atomic.h"
#include "common/snap_types.h"
#include <boost/function.hpp>
#include <list>

namespace librados {

class TestClassHandler;
class TestIoCtxImpl;
class TestRadosClient;

typedef boost::function<int(TestIoCtxImpl*,
			    const std::string&,
			    bufferlist *)> ObjectOperationTestImpl;
typedef std::list<ObjectOperationTestImpl> ObjectOperations;

struct TestObjectOperationImpl {
public:
  void get();
  void put();

  ObjectOperations ops;
private:
  atomic_t m_refcount;
};

class TestIoCtxImpl {
public:

  TestIoCtxImpl();
  explicit TestIoCtxImpl(TestRadosClient &client, int64_t m_pool_id,
                         const std::string& pool_name);

  TestRadosClient *get_rados_client() {
    return m_client;
  }

  void get();
  void put();

  virtual TestIoCtxImpl *clone() = 0;

  virtual uint64_t get_instance_id() const;
  virtual int64_t get_id();
  virtual uint64_t get_last_version();
  virtual std::string get_pool_name();
  snap_t get_snap_read() const {
    return m_snap_seq;
  }

  const SnapContext &get_snap_context() const {
    return m_snapc;
  }

  virtual int aio_flush();
  virtual void aio_flush_async(AioCompletionImpl *c);
  virtual int aio_operate(const std::string& oid, TestObjectOperationImpl &ops,
                          AioCompletionImpl *c, SnapContext *snap_context,
                          int flags);
  virtual int aio_operate_read(const std::string& oid, TestObjectOperationImpl &ops,
                               AioCompletionImpl *c, int flags,
                               bufferlist *pbl);
  virtual int aio_remove(const std::string& oid, AioCompletionImpl *c) = 0;

  virtual int assert_exists(const std::string &oid) = 0;

  virtual int create(const std::string& oid, bool exclusive) = 0;
  virtual int exec(const std::string& oid, TestClassHandler &handler,
                   const char *cls, const char *method,
                   bufferlist& inbl, bufferlist* outbl);
  virtual int list_snaps(const std::string& o, snap_set_t *out_snaps) = 0;
  virtual int list_watchers(const std::string& o,
                            std::list<obj_watch_t> *out_watchers);
  virtual int notify(const std::string& o, bufferlist& bl,
                     uint64_t timeout_ms, bufferlist *pbl);
  virtual void notify_ack(const std::string& o, uint64_t notify_id,
                          uint64_t handle, bufferlist& bl);
  virtual int omap_get_vals(const std::string& oid,
                            const std::string& start_after,
                            const std::string &filter_prefix,
                            uint64_t max_return,
                            std::map<std::string, bufferlist> *out_vals) = 0;
  virtual int omap_rm_keys(const std::string& oid,
                           const std::set<std::string>& keys) = 0;
  virtual int omap_set(const std::string& oid,
                       const std::map<std::string, bufferlist> &map) = 0;
  virtual int operate(const std::string& oid, TestObjectOperationImpl &ops);
  virtual int operate_read(const std::string& oid, TestObjectOperationImpl &ops,
                           bufferlist *pbl);
  virtual int read(const std::string& oid, size_t len, uint64_t off,
                   bufferlist *bl) = 0;
  virtual int remove(const std::string& oid) = 0;
  virtual int selfmanaged_snap_create(uint64_t *snapid) = 0;
  virtual int selfmanaged_snap_remove(uint64_t snapid) = 0;
  virtual int selfmanaged_snap_rollback(const std::string& oid,
                                        uint64_t snapid) = 0;
  virtual int selfmanaged_snap_set_write_ctx(snap_t seq,
                                             std::vector<snap_t>& snaps);
  virtual int set_alloc_hint(const std::string& oid,
                             uint64_t expected_object_size,
                             uint64_t expected_write_size);
  virtual void set_snap_read(snap_t seq);
  virtual int sparse_read(const std::string& oid, uint64_t off, uint64_t len,
                          std::map<uint64_t,uint64_t> *m,
                          bufferlist *data_bl) = 0;
  virtual int stat(const std::string& oid, uint64_t *psize, time_t *pmtime) = 0;
  virtual int truncate(const std::string& oid, uint64_t size) = 0;
  virtual int tmap_update(const std::string& oid, bufferlist& cmdbl);
  virtual int unwatch(uint64_t handle);
  virtual int watch(const std::string& o, uint64_t *handle,
                    librados::WatchCtx *ctx, librados::WatchCtx2 *ctx2);
  virtual int write(const std::string& oid, bufferlist& bl, size_t len,
                    uint64_t off) = 0;
  virtual int write_full(const std::string& oid, bufferlist& bl) = 0;
  virtual int xattr_get(const std::string& oid,
                        std::map<std::string, bufferlist>* attrset) = 0;
  virtual int xattr_set(const std::string& oid, const std::string &name,
                        bufferlist& bl) = 0;
  virtual int zero(const std::string& oid, uint64_t off, uint64_t len) = 0;

protected:
  TestIoCtxImpl(const TestIoCtxImpl& rhs);
  virtual ~TestIoCtxImpl();

  int execute_aio_operations(const std::string& oid, TestObjectOperationImpl *ops,
                             bufferlist *pbl);

private:

  TestRadosClient *m_client;
  int64_t m_pool_id;
  std::string m_pool_name;
  snap_t m_snap_seq;
  SnapContext m_snapc;
  atomic_t m_refcount;

};

} // namespace librados

#endif // CEPH_TEST_IO_CTX_IMPL_H
