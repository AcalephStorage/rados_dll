// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "include/rados/librados.hpp"
#include "common/ceph_argparse.h"
#include "common/common_init.h"
#include "common/config.h"
#include "common/snap_types.h"
#include "global/global_context.h"
#include "librados/AioCompletionImpl.h"
#include "test/librados_test_stub/TestClassHandler.h"
#include "test/librados_test_stub/TestIoCtxImpl.h"
#include "test/librados_test_stub/TestRadosClient.h"
#include "test/librados_test_stub/TestMemRadosClient.h"
#include "objclass/objclass.h"
#include <boost/bind.hpp>
#include <deque>
#include <list>
#include <vector>

static librados::TestClassHandler *get_class_handler() {
  static librados::TestClassHandler *s_class_handler = NULL;
  if (s_class_handler == NULL) {
    s_class_handler = new librados::TestClassHandler();
    s_class_handler->open_all_classes();
  }
  return s_class_handler;
}

static librados::TestRadosClient *get_rados_client() {
  // TODO: use factory to allow tests to swap out impl
  static librados::TestRadosClient *s_rados_client = NULL;
  if (s_rados_client == NULL) {
    CephInitParameters iparams(CEPH_ENTITY_TYPE_CLIENT);
    CephContext *cct = common_preinit(iparams, CODE_ENVIRONMENT_LIBRARY, 0);
    cct->_conf->parse_env();
    cct->_conf->apply_changes(NULL);
    g_ceph_context = cct;
    s_rados_client = new librados::TestMemRadosClient(cct);
    cct->put();
  }
  s_rados_client->get();
  return s_rados_client;
}

static void do_out_buffer(bufferlist& outbl, char **outbuf, size_t *outbuflen) {
  if (outbuf) {
    if (outbl.length() > 0) {
      *outbuf = (char *)malloc(outbl.length());
      memcpy(*outbuf, outbl.c_str(), outbl.length());
    } else {
      *outbuf = NULL;
    }
  }
  if (outbuflen) {
    *outbuflen = outbl.length();
  }
}

static void do_out_buffer(string& outbl, char **outbuf, size_t *outbuflen) {
  if (outbuf) {
    if (outbl.length() > 0) {
      *outbuf = (char *)malloc(outbl.length());
      memcpy(*outbuf, outbl.c_str(), outbl.length());
    } else {
      *outbuf = NULL;
    }
  }
  if (outbuflen) {
    *outbuflen = outbl.length();
  }
}

extern "C" int rados_aio_create_completion(void *cb_arg,
                                           rados_callback_t cb_complete,
                                           rados_callback_t cb_safe,
                                           rados_completion_t *pc)
{
  librados::AioCompletionImpl *c = new librados::AioCompletionImpl;
  if (cb_complete) {
    c->set_complete_callback(cb_arg, cb_complete);
  }
  if (cb_safe) {
    c->set_safe_callback(cb_arg, cb_safe);
  }
  *pc = c;
  return 0;
}

extern "C" int rados_aio_get_return_value(rados_completion_t c) {
  return reinterpret_cast<librados::AioCompletionImpl*>(c)->get_return_value();
}

extern "C" rados_config_t rados_cct(rados_t cluster)
{
  librados::TestRadosClient *client =
    reinterpret_cast<librados::TestRadosClient*>(cluster);
  return reinterpret_cast<rados_config_t>(client->cct());
}

extern "C" int rados_conf_parse_env(rados_t cluster, const char *var) {
  librados::TestRadosClient *client =
    reinterpret_cast<librados::TestRadosClient*>(cluster);
  md_config_t *conf = client->cct()->_conf;
  std::vector<const char*> args;
  env_to_vec(args, var);
  int ret = conf->parse_argv(args);
  if (ret == 0) {
    conf->apply_changes(NULL);
  }
  return ret;
}

extern "C" int rados_conf_read_file(rados_t cluster, const char *path) {
  librados::TestRadosClient *client =
    reinterpret_cast<librados::TestRadosClient*>(cluster);
  md_config_t *conf = client->cct()->_conf;
  std::deque<std::string> parse_errors;
  int ret = conf->parse_config_files(path, &parse_errors, NULL, 0);
  if (ret == 0) {
    conf->parse_env();
    conf->apply_changes(NULL);
    complain_about_parse_errors(client->cct(), &parse_errors);
  } else if (ret == -EINVAL) {
    // ignore missing client config
    return 0;
  }
  return ret;
}

extern "C" int rados_connect(rados_t cluster) {
  librados::TestRadosClient *client =
    reinterpret_cast<librados::TestRadosClient*>(cluster);
  return client->connect();
}

extern "C" int rados_create(rados_t *cluster, const char * const id) {
  *cluster = get_rados_client();
  return 0;
}

extern "C" int rados_ioctx_create(rados_t cluster, const char *pool_name,
                                  rados_ioctx_t *ioctx) {
  librados::TestRadosClient *client =
    reinterpret_cast<librados::TestRadosClient*>(cluster);

  int64_t pool_id = client->pool_lookup(pool_name);
  if (pool_id < 0) {
    return static_cast<int>(pool_id);
  }

  *ioctx = reinterpret_cast<rados_ioctx_t>(
      client->create_ioctx(pool_id, pool_name));
  return 0;
}

extern "C" int rados_ioctx_create2(rados_t cluster, int64_t pool_id,
                                   rados_ioctx_t *ioctx)
{
  librados::TestRadosClient *client =
    reinterpret_cast<librados::TestRadosClient*>(cluster);

  std::list<std::pair<int64_t, std::string> > pools;
  int r = client->pool_list(pools);
  if (r < 0) {
    return r;
  }

  for (std::list<std::pair<int64_t, std::string> >::iterator it =
       pools.begin(); it != pools.end(); ++it) {
    if (it->first == pool_id) {
      *ioctx = reinterpret_cast<rados_ioctx_t>(
	client->create_ioctx(pool_id, it->second));
      return 0;
    }
  }
  return -ENOENT;
}

extern "C" void rados_ioctx_destroy(rados_ioctx_t io) {
  librados::TestIoCtxImpl *ctx =
    reinterpret_cast<librados::TestIoCtxImpl*>(io);
  ctx->put();
}

extern "C" int rados_mon_command(rados_t cluster, const char **cmd,
                                 size_t cmdlen, const char *inbuf,
                                 size_t inbuflen, char **outbuf,
                                 size_t *outbuflen, char **outs,
                                 size_t *outslen) {
  librados::TestRadosClient *client =
    reinterpret_cast<librados::TestRadosClient*>(cluster);

  vector<string> cmdvec;
  for (size_t i = 0; i < cmdlen; i++) {
    cmdvec.push_back(cmd[i]);
  }

  bufferlist inbl;
  inbl.append(inbuf, inbuflen);

  bufferlist outbl;
  string outstring;
  int ret = client->mon_command(cmdvec, inbl, &outbl, &outstring);

  do_out_buffer(outbl, outbuf, outbuflen);
  do_out_buffer(outstring, outs, outslen);
  return ret;
}

extern "C" int rados_nobjects_list_open(rados_ioctx_t io,
                                        rados_list_ctx_t *ctx) {
  librados::TestIoCtxImpl *io_ctx =
    reinterpret_cast<librados::TestIoCtxImpl*>(io);
  librados::TestRadosClient *client = io_ctx->get_rados_client();

  std::list<librados::TestRadosClient::Object> *list =
    new std::list<librados::TestRadosClient::Object>();
  
  client->object_list(io_ctx->get_id(), list);
  list->push_front(librados::TestRadosClient::Object());
  *ctx = reinterpret_cast<rados_list_ctx_t>(list);
  return 0;
}

extern "C" int rados_nobjects_list_next(rados_list_ctx_t ctx,
                                        const char **entry,
                                        const char **key,
                                        const char **nspace) {
  std::list<librados::TestRadosClient::Object> *list =
    reinterpret_cast<std::list<librados::TestRadosClient::Object> *>(ctx);
  if (!list->empty()) {
    list->pop_front();
  }
  if (list->empty()) {
    return -ENOENT;
  }

  librados::TestRadosClient::Object &obj = list->front();
  if (entry != NULL) {
    *entry = obj.oid.c_str();
  }
  if (key != NULL) {
    *key = obj.locator.c_str();
  }
  if (nspace != NULL) {
    *nspace = obj.nspace.c_str();
  }
  return 0;
}

extern "C" void rados_nobjects_list_close(rados_list_ctx_t ctx) {
  std::list<librados::TestRadosClient::Object> *list =
    reinterpret_cast<std::list<librados::TestRadosClient::Object> *>(ctx);
  delete list;
}

extern "C" int rados_pool_create(rados_t cluster, const char *pool_name) {
  librados::TestRadosClient *client =
    reinterpret_cast<librados::TestRadosClient*>(cluster);
  return client->pool_create(pool_name);
}

extern "C" int rados_pool_delete(rados_t cluster, const char *pool_name) {
  librados::TestRadosClient *client =
    reinterpret_cast<librados::TestRadosClient*>(cluster);
  return client->pool_delete(pool_name);
}

extern "C" void rados_shutdown(rados_t cluster) {
  librados::TestRadosClient *client =
    reinterpret_cast<librados::TestRadosClient*>(cluster);
  client->put();
}

extern "C" int rados_wait_for_latest_osdmap(rados_t cluster) {
  librados::TestRadosClient *client =
    reinterpret_cast<librados::TestRadosClient*>(cluster);
  return client->wait_for_latest_osdmap();
}

namespace librados {

void AioCompletion::release() {
  AioCompletionImpl *c = reinterpret_cast<AioCompletionImpl *>(pc);
  c->release();
  delete this;
}

IoCtx::IoCtx() : io_ctx_impl(NULL) {
}

IoCtx::~IoCtx() {
  close();
}

int IoCtx::aio_flush() {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(io_ctx_impl);
  ctx->aio_flush();
  return 0;
}

int IoCtx::aio_flush_async(AioCompletion *c) {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(io_ctx_impl);
  ctx->aio_flush_async(c->pc);
  return 0;
}

int IoCtx::aio_operate(const std::string& oid, AioCompletion *c,
                       ObjectReadOperation *op, int flags,
                       bufferlist *pbl) {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(io_ctx_impl);
  TestObjectOperationImpl *ops = reinterpret_cast<TestObjectOperationImpl*>(op->impl);
  return ctx->aio_operate_read(oid, *ops, c->pc, flags, pbl);
}

int IoCtx::aio_operate(const std::string& oid, AioCompletion *c,
                       ObjectWriteOperation *op) {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(io_ctx_impl);
  TestObjectOperationImpl *ops = reinterpret_cast<TestObjectOperationImpl*>(op->impl);
  return ctx->aio_operate(oid, *ops, c->pc, NULL, 0);
}

int IoCtx::aio_operate(const std::string& oid, AioCompletion *c,
                       ObjectWriteOperation *op, snap_t seq,
                       std::vector<snap_t>& snaps) {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(io_ctx_impl);
  TestObjectOperationImpl *ops = reinterpret_cast<TestObjectOperationImpl*>(op->impl);

  std::vector<snapid_t> snv;
  snv.resize(snaps.size());
  for (size_t i = 0; i < snaps.size(); ++i)
    snv[i] = snaps[i];
  SnapContext snapc(seq, snv);

  return ctx->aio_operate(oid, *ops, c->pc, &snapc, 0);
}

int IoCtx::aio_remove(const std::string& oid, AioCompletion *c) {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(io_ctx_impl);
  return ctx->aio_remove(oid, c->pc);
}

config_t IoCtx::cct() {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(io_ctx_impl);
  return reinterpret_cast<config_t>(ctx->get_rados_client()->cct());
}

void IoCtx::close() {
  if (io_ctx_impl) {
    TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(io_ctx_impl);
    ctx->put();
  }
  io_ctx_impl = NULL;
}

int IoCtx::create(const std::string& oid, bool exclusive) {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(io_ctx_impl);
  return ctx->create(oid, exclusive);
}

void IoCtx::dup(const IoCtx& rhs) {
  close();
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(rhs.io_ctx_impl);
  io_ctx_impl = reinterpret_cast<IoCtxImpl*>(ctx->clone());
}

int IoCtx::exec(const std::string& oid, const char *cls, const char *method,
                bufferlist& inbl, bufferlist& outbl) {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(io_ctx_impl);
  return ctx->exec(oid, *get_class_handler(), cls, method, inbl, &outbl);
}

void IoCtx::from_rados_ioctx_t(rados_ioctx_t p, IoCtx &io) {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(p);
  ctx->get();

  io.close();
  io.io_ctx_impl = reinterpret_cast<IoCtxImpl*>(ctx);
}

uint64_t IoCtx::get_instance_id() const {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(io_ctx_impl);
  return ctx->get_instance_id();
}

int64_t IoCtx::get_id() {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(io_ctx_impl);
  return ctx->get_id();
}

uint64_t IoCtx::get_last_version() {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(io_ctx_impl);
  return ctx->get_last_version();
}

std::string IoCtx::get_pool_name() {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(io_ctx_impl);
  return ctx->get_pool_name();
}

int IoCtx::list_snaps(const std::string& o, snap_set_t *out_snaps) {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(io_ctx_impl);
  return ctx->list_snaps(o, out_snaps);
}

int IoCtx::list_watchers(const std::string& o,
                         std::list<obj_watch_t> *out_watchers) {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(io_ctx_impl);
  return ctx->list_watchers(o, out_watchers);
}

int IoCtx::notify(const std::string& o, uint64_t ver, bufferlist& bl) {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(io_ctx_impl);
  return ctx->notify(o, bl, 0, NULL);
}

int IoCtx::notify2(const std::string& o, bufferlist& bl,
                   uint64_t timeout_ms, bufferlist *pbl) {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(io_ctx_impl);
  return ctx->notify(o, bl, timeout_ms, pbl);
}

void IoCtx::notify_ack(const std::string& o, uint64_t notify_id,
                       uint64_t handle, bufferlist& bl) {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(io_ctx_impl);
  ctx->notify_ack(o, notify_id, handle, bl);
}

int IoCtx::omap_get_vals(const std::string& oid,
                         const std::string& start_after,
                         uint64_t max_return,
                         std::map<std::string, bufferlist> *out_vals) {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(io_ctx_impl);
  return ctx->omap_get_vals(oid, start_after, "", max_return, out_vals);
}

int IoCtx::operate(const std::string& oid, ObjectWriteOperation *op) {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(io_ctx_impl);
  TestObjectOperationImpl *ops = reinterpret_cast<TestObjectOperationImpl*>(op->impl);
  return ctx->operate(oid, *ops);
}

int IoCtx::operate(const std::string& oid, ObjectReadOperation *op,
                   bufferlist *pbl) {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(io_ctx_impl);
  TestObjectOperationImpl *ops = reinterpret_cast<TestObjectOperationImpl*>(op->impl);
  return ctx->operate_read(oid, *ops, pbl);
}

int IoCtx::read(const std::string& oid, bufferlist& bl, size_t len,
                uint64_t off) {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(io_ctx_impl);
  return ctx->read(oid, len, off, &bl);
}

int IoCtx::remove(const std::string& oid) {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(io_ctx_impl);
  return ctx->remove(oid);
}

int IoCtx::selfmanaged_snap_create(uint64_t *snapid) {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(io_ctx_impl);
  return ctx->selfmanaged_snap_create(snapid);
}

int IoCtx::selfmanaged_snap_remove(uint64_t snapid) {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(io_ctx_impl);
  return ctx->selfmanaged_snap_remove(snapid);
}

int IoCtx::selfmanaged_snap_rollback(const std::string& oid,
                                     uint64_t snapid) {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(io_ctx_impl);
  return ctx->selfmanaged_snap_rollback(oid, snapid);
}

int IoCtx::selfmanaged_snap_set_write_ctx(snap_t seq,
                                          std::vector<snap_t>& snaps) {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(io_ctx_impl);
  return ctx->selfmanaged_snap_set_write_ctx(seq, snaps);
}

void IoCtx::snap_set_read(snap_t seq) {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(io_ctx_impl);
  ctx->set_snap_read(seq);
}

int IoCtx::stat(const std::string& oid, uint64_t *psize, time_t *pmtime) {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(io_ctx_impl);
  return ctx->stat(oid, psize, pmtime);;
}

int IoCtx::tmap_update(const std::string& oid, bufferlist& cmdbl) {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(io_ctx_impl);
  return ctx->tmap_update(oid, cmdbl);
}

int IoCtx::unwatch2(uint64_t handle) {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(io_ctx_impl);
  return ctx->unwatch(handle);
}

int IoCtx::unwatch(const std::string& o, uint64_t handle) {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(io_ctx_impl);
  return ctx->unwatch(handle);
}

int IoCtx::watch(const std::string& o, uint64_t ver, uint64_t *handle,
                 librados::WatchCtx *wctx) {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(io_ctx_impl);
  return ctx->watch(o, handle, wctx, NULL);
}

int IoCtx::watch2(const std::string& o, uint64_t *handle,
                  librados::WatchCtx2 *wctx) {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(io_ctx_impl);
  return ctx->watch(o, handle, NULL, wctx);
}

int IoCtx::write(const std::string& oid, bufferlist& bl, size_t len,
                 uint64_t off) {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(io_ctx_impl);
  return ctx->write(oid, bl, len, off);
}

static int save_operation_result(int result, int *pval) {
  if (pval != NULL) {
    *pval = result;
  }
  return result;
}

ObjectOperation::ObjectOperation() {
  TestObjectOperationImpl *o = new TestObjectOperationImpl();
  o->get();
  impl = reinterpret_cast<ObjectOperationImpl*>(o);
}

ObjectOperation::~ObjectOperation() {
  TestObjectOperationImpl *o = reinterpret_cast<TestObjectOperationImpl*>(impl);
  if (o) {
    o->put();
    o = NULL;
  }
}

void ObjectOperation::assert_exists() {
  TestObjectOperationImpl *o = reinterpret_cast<TestObjectOperationImpl*>(impl);
  o->ops.push_back(boost::bind(&TestIoCtxImpl::assert_exists, _1, _2));
}

void ObjectOperation::exec(const char *cls, const char *method,
                           bufferlist& inbl) {
  TestObjectOperationImpl *o = reinterpret_cast<TestObjectOperationImpl*>(impl);
  o->ops.push_back(boost::bind(&TestIoCtxImpl::exec, _1, _2,
			       boost::ref(*get_class_handler()),
			       cls, method, inbl, _3));
}

void ObjectOperation::set_op_flags2(int flags) {
}

size_t ObjectOperation::size() {
  TestObjectOperationImpl *o = reinterpret_cast<TestObjectOperationImpl*>(impl);
  return o->ops.size();
}

void ObjectReadOperation::read(size_t off, uint64_t len, bufferlist *pbl,
                               int *prval) {
  TestObjectOperationImpl *o = reinterpret_cast<TestObjectOperationImpl*>(impl);

  ObjectOperationTestImpl op;
  if (pbl != NULL) {
    op = boost::bind(&TestIoCtxImpl::read, _1, _2, len, off, pbl);
  } else {
    op = boost::bind(&TestIoCtxImpl::read, _1, _2, len, off, _3);
  }

  if (prval != NULL) {
    op = boost::bind(save_operation_result,
                     boost::bind(op, _1, _2, _3), prval);
  }
  o->ops.push_back(op);
}

void ObjectReadOperation::sparse_read(uint64_t off, uint64_t len,
                                      std::map<uint64_t,uint64_t> *m,
                                      bufferlist *pbl, int *prval) {
  TestObjectOperationImpl *o = reinterpret_cast<TestObjectOperationImpl*>(impl);

  ObjectOperationTestImpl op;
  if (pbl != NULL) {
    op = boost::bind(&TestIoCtxImpl::sparse_read, _1, _2, off, len, m, pbl);
  } else {
    op = boost::bind(&TestIoCtxImpl::sparse_read, _1, _2, off, len, m, _3);
  }

  if (prval != NULL) {
    op = boost::bind(save_operation_result,
                     boost::bind(op, _1, _2, _3), prval);
  }
  o->ops.push_back(op);
}

void ObjectWriteOperation::create(bool exclusive) {
  TestObjectOperationImpl *o = reinterpret_cast<TestObjectOperationImpl*>(impl);
  o->ops.push_back(boost::bind(&TestIoCtxImpl::create, _1, _2, exclusive));
}

void ObjectWriteOperation::omap_set(const std::map<std::string, bufferlist> &map) {
  TestObjectOperationImpl *o = reinterpret_cast<TestObjectOperationImpl*>(impl);
  o->ops.push_back(boost::bind(&TestIoCtxImpl::omap_set, _1, _2, boost::ref(map)));
}

void ObjectWriteOperation::remove() {
  TestObjectOperationImpl *o = reinterpret_cast<TestObjectOperationImpl*>(impl);
  o->ops.push_back(boost::bind(&TestIoCtxImpl::remove, _1, _2));
}

void ObjectWriteOperation::selfmanaged_snap_rollback(uint64_t snapid) {
  TestObjectOperationImpl *o = reinterpret_cast<TestObjectOperationImpl*>(impl);
  o->ops.push_back(boost::bind(&TestIoCtxImpl::selfmanaged_snap_rollback,
			       _1, _2, snapid));
}

void ObjectWriteOperation::set_alloc_hint(uint64_t expected_object_size,
                                          uint64_t expected_write_size) {
  TestObjectOperationImpl *o = reinterpret_cast<TestObjectOperationImpl*>(impl);
  o->ops.push_back(boost::bind(&TestIoCtxImpl::set_alloc_hint, _1, _2,
			       expected_object_size, expected_write_size));
}

void ObjectWriteOperation::truncate(uint64_t off) {
  TestObjectOperationImpl *o = reinterpret_cast<TestObjectOperationImpl*>(impl);
  o->ops.push_back(boost::bind(&TestIoCtxImpl::truncate, _1, _2, off));
}

void ObjectWriteOperation::write(uint64_t off, const bufferlist& bl) {
  TestObjectOperationImpl *o = reinterpret_cast<TestObjectOperationImpl*>(impl);
  o->ops.push_back(boost::bind(&TestIoCtxImpl::write, _1, _2, bl, bl.length(),
			       off));
}

void ObjectWriteOperation::write_full(const bufferlist& bl) {
  TestObjectOperationImpl *o = reinterpret_cast<TestObjectOperationImpl*>(impl);
  o->ops.push_back(boost::bind(&TestIoCtxImpl::write_full, _1, _2, bl));
}

void ObjectWriteOperation::zero(uint64_t off, uint64_t len) {
  TestObjectOperationImpl *o = reinterpret_cast<TestObjectOperationImpl*>(impl);
  o->ops.push_back(boost::bind(&TestIoCtxImpl::zero, _1, _2, off, len));
}

Rados::Rados() : client(NULL) {
}

Rados::Rados(IoCtx& ioctx) {
  TestIoCtxImpl *ctx = reinterpret_cast<TestIoCtxImpl*>(ioctx.io_ctx_impl);
  TestRadosClient *impl = ctx->get_rados_client();
  impl->get();

  client = reinterpret_cast<RadosClient*>(impl);
  assert(client != NULL);
}

Rados::~Rados() {
  shutdown();
}

AioCompletion *Rados::aio_create_completion(void *cb_arg,
                                            callback_t cb_complete,
                                            callback_t cb_safe) {
  AioCompletionImpl *c;
  int r = rados_aio_create_completion(cb_arg, cb_complete, cb_safe,
      reinterpret_cast<void**>(&c));
  assert(r == 0);
  return new AioCompletion(c);
}

int Rados::blacklist_add(const std::string& client_address,
			 uint32_t expire_seconds) {
  TestRadosClient *impl = reinterpret_cast<TestRadosClient*>(client);
  return impl->blacklist_add(client_address, expire_seconds);
}

int Rados::conf_parse_env(const char *env) const {
  return rados_conf_parse_env(reinterpret_cast<rados_t>(client), env);
}

int Rados::conf_read_file(const char * const path) const {
  return rados_conf_read_file(reinterpret_cast<rados_t>(client), path);
}

int Rados::connect() {
  return rados_connect(reinterpret_cast<rados_t>(client));
}

uint64_t Rados::get_instance_id() {
  TestRadosClient *impl = reinterpret_cast<TestRadosClient*>(client);
  return impl->get_instance_id();
}

int Rados::init(const char * const id) {
  return rados_create(reinterpret_cast<rados_t *>(&client), id);
}

int Rados::ioctx_create(const char *name, IoCtx &io) {
  rados_ioctx_t p;
  int ret = rados_ioctx_create(reinterpret_cast<rados_t>(client), name, &p);
  if (ret) {
    return ret;
  }
  io.io_ctx_impl = reinterpret_cast<IoCtxImpl*>(p);
  return 0;
}

int Rados::ioctx_create2(int64_t pool_id, IoCtx &io)
{
  rados_ioctx_t p;
  int ret = rados_ioctx_create2(reinterpret_cast<rados_t>(client), pool_id, &p);
  if (ret) {
    return ret;
  }
  io.io_ctx_impl = reinterpret_cast<IoCtxImpl*>(p);
  return 0;
}

int Rados::mon_command(std::string cmd, const bufferlist& inbl,
                       bufferlist *outbl, std::string *outs) {
  TestRadosClient *impl = reinterpret_cast<TestRadosClient*>(client);

  std::vector<std::string> cmds;
  cmds.push_back(cmd);
  return impl->mon_command(cmds, inbl, outbl, outs);
}

int Rados::pool_create(const char *name) {
  TestRadosClient *impl = reinterpret_cast<TestRadosClient*>(client);
  return impl->pool_create(name);
}

int Rados::pool_delete(const char *name) {
  TestRadosClient *impl = reinterpret_cast<TestRadosClient*>(client);
  return impl->pool_delete(name);
}

int Rados::pool_get_base_tier(int64_t pool, int64_t* base_tier) {
  TestRadosClient *impl = reinterpret_cast<TestRadosClient*>(client);
  return impl->pool_get_base_tier(pool, base_tier);
}

int Rados::pool_list(std::list<std::string>& v) {
  TestRadosClient *impl = reinterpret_cast<TestRadosClient*>(client);
  std::list<std::pair<int64_t, std::string> > pools;
  int r = impl->pool_list(pools);
  if (r < 0) {
    return r;
  }

  v.clear();
  for (std::list<std::pair<int64_t, std::string> >::iterator it = pools.begin();
       it != pools.end(); ++it) {
    v.push_back(it->second);
  }
  return 0;
}

int Rados::pool_list2(std::list<std::pair<int64_t, std::string> >& v)
{
  TestRadosClient *impl = reinterpret_cast<TestRadosClient*>(client);
  return impl->pool_list(v);
}

int64_t Rados::pool_lookup(const char *name) {
  TestRadosClient *impl = reinterpret_cast<TestRadosClient*>(client);
  return impl->pool_lookup(name);
}

int Rados::pool_reverse_lookup(int64_t id, std::string *name) {
  TestRadosClient *impl = reinterpret_cast<TestRadosClient*>(client);
  return impl->pool_reverse_lookup(id, name);
}

void Rados::shutdown() {
  if (client == NULL) {
    return;
  }
  TestRadosClient *impl = reinterpret_cast<TestRadosClient*>(client);
  impl->put();
  client = NULL;
}

void Rados::test_blacklist_self(bool set) {
}

int Rados::wait_for_latest_osdmap() {
  TestRadosClient *impl = reinterpret_cast<TestRadosClient*>(client);
  return impl->wait_for_latest_osdmap();
}

int Rados::watch_flush() {
  TestRadosClient *impl = reinterpret_cast<TestRadosClient*>(client);
  return impl->watch_flush();
}

WatchCtx::~WatchCtx() {
}

WatchCtx2::~WatchCtx2() {
}

} // namespace librados

int cls_cxx_create(cls_method_context_t hctx, bool exclusive) {
  librados::TestClassHandler::MethodContext *ctx =
    reinterpret_cast<librados::TestClassHandler::MethodContext*>(hctx);
  return ctx->io_ctx_impl->create(ctx->oid, exclusive);
}

int cls_get_request_origin(cls_method_context_t hctx, entity_inst_t *origin) {
  //TODO
  return 0;
}

int cls_cxx_getxattr(cls_method_context_t hctx, const char *name,
                     bufferlist *outbl) {
  std::map<string, bufferlist> attrs;
  int r = cls_cxx_getxattrs(hctx, &attrs);
  if (r < 0) {
    return r;
  }

  std::map<string, bufferlist>::iterator it = attrs.find(name);
  if (it == attrs.end()) {
    return -ENODATA;
  }
  *outbl = it->second;
  return 0;
}

int cls_cxx_getxattrs(cls_method_context_t hctx, std::map<string, bufferlist> *attrset) {
  librados::TestClassHandler::MethodContext *ctx =
    reinterpret_cast<librados::TestClassHandler::MethodContext*>(hctx);
  return ctx->io_ctx_impl->xattr_get(ctx->oid, attrset);
}

int cls_cxx_map_get_keys(cls_method_context_t hctx, const string &start_obj,
                         uint64_t max_to_get, std::set<string> *keys) {
  librados::TestClassHandler::MethodContext *ctx =
    reinterpret_cast<librados::TestClassHandler::MethodContext*>(hctx);

  keys->clear();
  std::map<string, bufferlist> vals;
  std::string last_key = start_obj;
  do {
    vals.clear();
    int r = ctx->io_ctx_impl->omap_get_vals(ctx->oid, last_key, "", 1024,
                                            &vals);
    if (r < 0) {
      return r;
    }

    for (std::map<string, bufferlist>::iterator it = vals.begin();
        it != vals.end(); ++it) {
      last_key = it->first;
      keys->insert(last_key);
    }
  } while (!vals.empty());
  return 0;
}

int cls_cxx_map_get_val(cls_method_context_t hctx, const string &key,
                        bufferlist *outbl) {
  librados::TestClassHandler::MethodContext *ctx =
    reinterpret_cast<librados::TestClassHandler::MethodContext*>(hctx);

  std::map<string, bufferlist> vals;
  int r = ctx->io_ctx_impl->omap_get_vals(ctx->oid, "", key, 1024, &vals);
  if (r < 0) {
    return r;
  }

  std::map<string, bufferlist>::iterator it = vals.find(key);
  if (it == vals.end()) {
    return -ENOENT;
  }

  *outbl = it->second;
  return 0;
}

int cls_cxx_map_get_vals(cls_method_context_t hctx, const string &start_obj,
                         const string &filter_prefix, uint64_t max_to_get,
                         std::map<string, bufferlist> *vals) {
  librados::TestClassHandler::MethodContext *ctx =
    reinterpret_cast<librados::TestClassHandler::MethodContext*>(hctx);
  return ctx->io_ctx_impl->omap_get_vals(ctx->oid, start_obj, filter_prefix,
      max_to_get, vals);
}

int cls_cxx_map_remove_key(cls_method_context_t hctx, const string &key) {
  std::set<std::string> keys;
  keys.insert(key);

  librados::TestClassHandler::MethodContext *ctx =
    reinterpret_cast<librados::TestClassHandler::MethodContext*>(hctx);
  return ctx->io_ctx_impl->omap_rm_keys(ctx->oid, keys);
}

int cls_cxx_map_set_val(cls_method_context_t hctx, const string &key,
                        bufferlist *inbl) {
  std::map<std::string, bufferlist> m;
  m[key] = *inbl;
  return cls_cxx_map_set_vals(hctx, &m);
}

int cls_cxx_map_set_vals(cls_method_context_t hctx,
                         const std::map<string, bufferlist> *map) {
  librados::TestClassHandler::MethodContext *ctx =
    reinterpret_cast<librados::TestClassHandler::MethodContext*>(hctx);
  return ctx->io_ctx_impl->omap_set(ctx->oid, *map);
}

int cls_cxx_read(cls_method_context_t hctx, int ofs, int len,
                 bufferlist *outbl) {
  librados::TestClassHandler::MethodContext *ctx =
    reinterpret_cast<librados::TestClassHandler::MethodContext*>(hctx);
  return ctx->io_ctx_impl->read(ctx->oid, len, ofs, outbl);
}

int cls_cxx_setxattr(cls_method_context_t hctx, const char *name,
                     bufferlist *inbl) {
  librados::TestClassHandler::MethodContext *ctx =
    reinterpret_cast<librados::TestClassHandler::MethodContext*>(hctx);
  return ctx->io_ctx_impl->xattr_set(ctx->oid, name, *inbl);
}

int cls_cxx_stat(cls_method_context_t hctx, uint64_t *size, time_t *mtime) {
  librados::TestClassHandler::MethodContext *ctx =
    reinterpret_cast<librados::TestClassHandler::MethodContext*>(hctx);
  return ctx->io_ctx_impl->stat(ctx->oid, size, mtime);
}

int cls_cxx_write(cls_method_context_t hctx, int ofs, int len,
                  bufferlist *inbl) {
  librados::TestClassHandler::MethodContext *ctx =
    reinterpret_cast<librados::TestClassHandler::MethodContext*>(hctx);
  return ctx->io_ctx_impl->write(ctx->oid, *inbl, len, ofs);
}

int cls_cxx_write_full(cls_method_context_t hctx, bufferlist *inbl) {
  librados::TestClassHandler::MethodContext *ctx =
    reinterpret_cast<librados::TestClassHandler::MethodContext*>(hctx);
  return ctx->io_ctx_impl->write_full(ctx->oid, *inbl);
}

int cls_log(int level, const char *format, ...) {
  return 0;
}

int cls_register(const char *name, cls_handle_t *handle) {
  librados::TestClassHandler *cls = get_class_handler();
  return cls->create(name, handle);
}

int cls_register_cxx_method(cls_handle_t hclass, const char *method,
    int flags,
    cls_method_cxx_call_t class_call,
    cls_method_handle_t *handle) {
  librados::TestClassHandler *cls = get_class_handler();
  return cls->create_method(hclass, method, class_call, handle);
}
