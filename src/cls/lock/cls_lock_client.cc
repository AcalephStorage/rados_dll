// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#include "include/types.h"
#include "msg/msg_types.h"
#include "include/rados/librados.hpp"

using namespace librados;

#include <iostream>

#include <errno.h>
#include <stdlib.h>
#include <time.h>

#include "cls/lock/cls_lock_types.h"
#include "cls/lock/cls_lock_ops.h"
#include "cls/lock/cls_lock_client.h"

namespace rados {
  namespace cls {
    namespace lock {

      void lock(ObjectWriteOperation *rados_op,
                const string& name, ClsLockType type,
                const string& cookie, const string& tag,
                const string& description,
                const utime_t& duration, uint8_t flags)
      {
        cls_lock_lock_op op;
        op.name = name;
        op.type = type;
        op.cookie = cookie;
        op.tag = tag;
        op.description = description;
        op.duration = duration;
        op.flags = flags;
        bufferlist in;
        ::encode(op, in);
        rados_op->exec("lock", "lock", in);
      }

      int lock(IoCtx *ioctx,
               const string& oid,
               const string& name, ClsLockType type,
               const string& cookie, const string& tag,
               const string& description, const utime_t& duration,
	       uint8_t flags)
      {
        ObjectWriteOperation op;
        lock(&op, name, type, cookie, tag, description, duration, flags);
        return ioctx->operate(oid, &op);
      }

      void unlock(ObjectWriteOperation *rados_op,
                  const string& name, const string& cookie)
      {
        cls_lock_unlock_op op;
        op.name = name;
        op.cookie = cookie;
        bufferlist in;
        ::encode(op, in);

        rados_op->exec("lock", "unlock", in);
      }

      int unlock(IoCtx *ioctx, const string& oid,
                 const string& name, const string& cookie)
      {
        ObjectWriteOperation op;
        unlock(&op, name, cookie);
        return ioctx->operate(oid, &op);
      }

      void break_lock(ObjectWriteOperation *rados_op,
                      const string& name, const string& cookie,
                      const entity_name_t& locker)
      {
        cls_lock_break_op op;
        op.name = name;
        op.cookie = cookie;
        op.locker = locker;
        bufferlist in;
        ::encode(op, in);
        rados_op->exec("lock", "break_lock", in);
      }

      int break_lock(IoCtx *ioctx, const string& oid,
                     const string& name, const string& cookie,
                     const entity_name_t& locker)
      {
        ObjectWriteOperation op;
        break_lock(&op, name, cookie, locker);
        return ioctx->operate(oid, &op);
      }

      int list_locks(IoCtx *ioctx, const string& oid, list<string> *locks)
      {
        bufferlist in, out;
        int r = ioctx->exec(oid, "lock", "list_locks", in, out);
        if (r < 0)
          return r;

        cls_lock_list_locks_reply ret;
        bufferlist::iterator iter = out.begin();
        try {
          ::decode(ret, iter);
        } catch (buffer::error& err) {
	  return -EBADMSG;
        }

        *locks = ret.locks;

        return 0;
      }

      void get_lock_info_start(ObjectReadOperation *rados_op,
			       const string& name)
      {
        bufferlist in;
        cls_lock_get_info_op op;
        op.name = name;
        ::encode(op, in);
        rados_op->exec("lock", "get_info", in);
      }

      int get_lock_info_finish(bufferlist::iterator *iter,
			       map<locker_id_t, locker_info_t> *lockers,
			       ClsLockType *type, string *tag)
      {
        cls_lock_get_info_reply ret;
        try {
          ::decode(ret, *iter);
        } catch (buffer::error& err) {
	  return -EBADMSG;
        }

        if (lockers) {
          *lockers = ret.lockers;
        }

        if (type) {
          *type = ret.lock_type;
        }

        if (tag) {
          *tag = ret.tag;
        }

        return 0;
      }

      int get_lock_info(IoCtx *ioctx, const string& oid, const string& name,
                        map<locker_id_t, locker_info_t> *lockers,
                        ClsLockType *type, string *tag)
      {
        ObjectReadOperation op;
        get_lock_info_start(&op, name);
	bufferlist out;
        int r = ioctx->operate(oid, &op, &out);
	if (r < 0)
	  return r;
	bufferlist::iterator it = out.begin();
	return get_lock_info_finish(&it, lockers, type, tag);
      }

      void assert_locked(librados::ObjectOperation *rados_op,
                         const std::string& name, ClsLockType type,
                         const std::string& cookie, const std::string& tag)
      {
        cls_lock_assert_op op;
        op.name = name;
        op.type = type;
        op.cookie = cookie;
        op.tag = tag;
        bufferlist in;
        ::encode(op, in);
        rados_op->exec("lock", "assert_locked", in);
      }

      void Lock::assert_locked_exclusive(ObjectOperation *op)
      {
        assert_locked(op, name, LOCK_EXCLUSIVE, cookie, tag);
      }

      void Lock::assert_locked_shared(ObjectOperation *op)
      {
        assert_locked(op, name, LOCK_SHARED, cookie, tag);
      }

      void Lock::lock_shared(ObjectWriteOperation *op)
      {
        lock(op, name, LOCK_SHARED,
             cookie, tag, description, duration, flags);
      }

      int Lock::lock_shared(IoCtx *ioctx, const string& oid)
      {
        return lock(ioctx, oid, name, LOCK_SHARED,
                    cookie, tag, description, duration, flags);
      }

      void Lock::lock_exclusive(ObjectWriteOperation *op)
      {
        lock(op, name, LOCK_EXCLUSIVE,
             cookie, tag, description, duration, flags);
      }

      int Lock::lock_exclusive(IoCtx *ioctx, const string& oid)
      {
        return lock(ioctx, oid, name, LOCK_EXCLUSIVE,
                    cookie, tag, description, duration, flags);
      }

      void Lock::unlock(ObjectWriteOperation *op)
      {
	rados::cls::lock::unlock(op, name, cookie);
      }

      int Lock::unlock(IoCtx *ioctx, const string& oid)
      {
        return rados::cls::lock::unlock(ioctx, oid, name, cookie);
      }

      void Lock::break_lock(ObjectWriteOperation *op, const entity_name_t& locker)
      {
	rados::cls::lock::break_lock(op, name, cookie, locker);
      }

      int Lock::break_lock(IoCtx *ioctx, const string& oid, const entity_name_t& locker)
      {
          return rados::cls::lock::break_lock(ioctx, oid, name, cookie, locker);
      }
    } // namespace lock
  } // namespace cls
} // namespace rados

