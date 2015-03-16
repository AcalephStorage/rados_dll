// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2013 Inktank, Inc
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */


#include "include/utime.h"
#include "common/lru_map.h"
#include "common/RefCountedObj.h"
#include "common/Thread.h"
#include "common/Mutex.h"
#include "common/RWLock.h"

#include "rgw_common.h"
#include "rgw_rados.h"
#include "rgw_quota.h"
#include "rgw_bucket.h"
#include "rgw_user.h"

#define dout_subsys ceph_subsys_rgw


struct RGWQuotaCacheStats {
  RGWStorageStats stats;
  utime_t expiration;
  utime_t async_refresh_time;
};

template<class T>
class RGWQuotaCache {
protected:
  RGWRados *store;
  lru_map<T, RGWQuotaCacheStats> stats_map;
  RefCountedWaitObject *async_refcount;

  class StatsAsyncTestSet : public lru_map<T, RGWQuotaCacheStats>::UpdateContext {
    int objs_delta;
    uint64_t added_bytes;
    uint64_t removed_bytes;
  public:
    StatsAsyncTestSet() {}
    bool update(RGWQuotaCacheStats *entry) {
      if (entry->async_refresh_time.sec() == 0)
        return false;

      entry->async_refresh_time = utime_t(0, 0);

      return true;
    }
  };

  virtual int fetch_stats_from_storage(const string& user, rgw_bucket& bucket, RGWStorageStats& stats) = 0;

  virtual bool map_find(const string& user, rgw_bucket& bucket, RGWQuotaCacheStats& qs) = 0;

  virtual bool map_find_and_update(const string& user, rgw_bucket& bucket, typename lru_map<T, RGWQuotaCacheStats>::UpdateContext *ctx) = 0;
  virtual void map_add(const string& user, rgw_bucket& bucket, RGWQuotaCacheStats& qs) = 0;

  virtual void data_modified(const string& user, rgw_bucket& bucket) {}
public:
  RGWQuotaCache(RGWRados *_store, int size) : store(_store), stats_map(size) {
    async_refcount = new RefCountedWaitObject;
  }
  virtual ~RGWQuotaCache() {
    async_refcount->put_wait(); /* wait for all pending async requests to complete */
  }

  int get_stats(const string& user, rgw_bucket& bucket, RGWStorageStats& stats, RGWQuotaInfo& quota);
  void adjust_stats(const string& user, rgw_bucket& bucket, int objs_delta, uint64_t added_bytes, uint64_t removed_bytes);

  virtual bool can_use_cached_stats(RGWQuotaInfo& quota, RGWStorageStats& stats);

  void set_stats(const string& user, rgw_bucket& bucket, RGWQuotaCacheStats& qs, RGWStorageStats& stats);
  int async_refresh(const string& user, rgw_bucket& bucket, RGWQuotaCacheStats& qs);
  void async_refresh_response(const string& user, rgw_bucket& bucket, RGWStorageStats& stats);

  class AsyncRefreshHandler {
  protected:
    RGWRados *store;
    RGWQuotaCache<T> *cache;
  public:
    AsyncRefreshHandler(RGWRados *_store, RGWQuotaCache<T> *_cache) : store(_store), cache(_cache) {}
    virtual ~AsyncRefreshHandler() {}

    virtual int init_fetch() = 0;
    virtual void drop_reference() = 0;
  };

  virtual AsyncRefreshHandler *allocate_refresh_handler(const string& user, rgw_bucket& bucket) = 0;
};

template<class T>
bool RGWQuotaCache<T>::can_use_cached_stats(RGWQuotaInfo& quota, RGWStorageStats& cached_stats)
{
  if (quota.max_size_kb >= 0) {
    if (quota.max_size_soft_threshold < 0) {
      quota.max_size_soft_threshold = quota.max_size_kb * store->ctx()->_conf->rgw_bucket_quota_soft_threshold;
    }

    if (cached_stats.num_kb_rounded >= (uint64_t)quota.max_size_soft_threshold) {
      ldout(store->ctx(), 20) << "quota: can't use cached stats, exceeded soft threshold (size): "
        << cached_stats.num_kb_rounded << " >= " << quota.max_size_soft_threshold << dendl;
      return false;
    }
  }

  if (quota.max_objects >= 0) {
    if (quota.max_objs_soft_threshold < 0) {
      quota.max_objs_soft_threshold = quota.max_objects * store->ctx()->_conf->rgw_bucket_quota_soft_threshold;
    }

    if (cached_stats.num_objects >= (uint64_t)quota.max_objs_soft_threshold) {
      ldout(store->ctx(), 20) << "quota: can't use cached stats, exceeded soft threshold (num objs): "
        << cached_stats.num_objects << " >= " << quota.max_objs_soft_threshold << dendl;
      return false;
    }
  }

  return true;
}

template<class T>
int RGWQuotaCache<T>::async_refresh(const string& user, rgw_bucket& bucket, RGWQuotaCacheStats& qs)
{
  /* protect against multiple updates */
  StatsAsyncTestSet test_update;
  if (!map_find_and_update(user, bucket, &test_update)) {
    /* most likely we just raced with another update */
    return 0;
  }

  async_refcount->get();


  AsyncRefreshHandler *handler = allocate_refresh_handler(user, bucket);

  int ret = handler->init_fetch();
  if (ret < 0) {
    async_refcount->put();
    handler->drop_reference();
    return ret;
  }

  return 0;
}

template<class T>
void RGWQuotaCache<T>::async_refresh_response(const string& user, rgw_bucket& bucket, RGWStorageStats& stats)
{
  ldout(store->ctx(), 20) << "async stats refresh response for bucket=" << bucket << dendl;

  RGWQuotaCacheStats qs;

  map_find(user, bucket, qs);

  set_stats(user, bucket, qs, stats);

  async_refcount->put();
}

template<class T>
void RGWQuotaCache<T>::set_stats(const string& user, rgw_bucket& bucket, RGWQuotaCacheStats& qs, RGWStorageStats& stats)
{
  qs.stats = stats;
  qs.expiration = ceph_clock_now(store->ctx());
  qs.async_refresh_time = qs.expiration;
  qs.expiration += store->ctx()->_conf->rgw_bucket_quota_ttl;
  qs.async_refresh_time += store->ctx()->_conf->rgw_bucket_quota_ttl / 2;

  map_add(user, bucket, qs);
}

template<class T>
int RGWQuotaCache<T>::get_stats(const string& user, rgw_bucket& bucket, RGWStorageStats& stats, RGWQuotaInfo& quota) {
  RGWQuotaCacheStats qs;
  utime_t now = ceph_clock_now(store->ctx());
  if (map_find(user, bucket, qs)) {
    if (qs.async_refresh_time.sec() > 0 && now >= qs.async_refresh_time) {
      int r = async_refresh(user, bucket, qs);
      if (r < 0) {
        ldout(store->ctx(), 0) << "ERROR: quota async refresh returned ret=" << r << dendl;

        /* continue processing, might be a transient error, async refresh is just optimization */
      }
    }

    if (can_use_cached_stats(quota, qs.stats) && qs.expiration > ceph_clock_now(store->ctx())) {
      stats = qs.stats;
      return 0;
    }
  }

  int ret = fetch_stats_from_storage(user, bucket, stats);
  if (ret < 0 && ret != -ENOENT)
    return ret;

  set_stats(user, bucket, qs, stats);

  return 0;
}


template<class T>
class RGWQuotaStatsUpdate : public lru_map<T, RGWQuotaCacheStats>::UpdateContext {
  int objs_delta;
  uint64_t added_bytes;
  uint64_t removed_bytes;
public:
  RGWQuotaStatsUpdate(int _objs_delta, uint64_t _added_bytes, uint64_t _removed_bytes) : 
                    objs_delta(_objs_delta), added_bytes(_added_bytes), removed_bytes(_removed_bytes) {}
  bool update(RGWQuotaCacheStats *entry) {
    uint64_t rounded_kb_added = rgw_rounded_objsize_kb(added_bytes);
    uint64_t rounded_kb_removed = rgw_rounded_objsize_kb(removed_bytes);

    entry->stats.num_kb_rounded += (rounded_kb_added - rounded_kb_removed);
    entry->stats.num_kb += (added_bytes - removed_bytes) / 1024;
    entry->stats.num_objects += objs_delta;

    return true;
  }
};


template<class T>
void RGWQuotaCache<T>::adjust_stats(const string& user, rgw_bucket& bucket, int objs_delta,
                                 uint64_t added_bytes, uint64_t removed_bytes)
{
  RGWQuotaStatsUpdate<T> update(objs_delta, added_bytes, removed_bytes);
  map_find_and_update(user, bucket, &update);

  data_modified(user, bucket);
}

class BucketAsyncRefreshHandler : public RGWQuotaCache<rgw_bucket>::AsyncRefreshHandler,
                                  public RGWGetBucketStats_CB {
  string user;
public:
  BucketAsyncRefreshHandler(RGWRados *_store, RGWQuotaCache<rgw_bucket> *_cache,
                            const string& _user, rgw_bucket& _bucket) :
                                      RGWQuotaCache<rgw_bucket>::AsyncRefreshHandler(_store, _cache),
                                      RGWGetBucketStats_CB(_bucket), user(_user) {}

  void drop_reference() { put(); }
  void handle_response(int r);
  int init_fetch();
};

int BucketAsyncRefreshHandler::init_fetch()
{
  ldout(store->ctx(), 20) << "initiating async quota refresh for bucket=" << bucket << dendl;

  int r = store->get_bucket_stats_async(bucket, this);
  if (r < 0) {
    ldout(store->ctx(), 0) << "could not get bucket info for bucket=" << bucket.name << dendl;

    /* get_bucket_stats_async() dropped our reference already */
    return r;
  }

  return 0;
}

void BucketAsyncRefreshHandler::handle_response(int r)
{
  if (r < 0) {
    ldout(store->ctx(), 20) << "AsyncRefreshHandler::handle_response() r=" << r << dendl;
    return; /* nothing to do here */
  }

  RGWStorageStats bs;

  map<RGWObjCategory, RGWStorageStats>::iterator iter;
  for (iter = stats->begin(); iter != stats->end(); ++iter) {
    RGWStorageStats& s = iter->second;
    bs.num_kb += s.num_kb;
    bs.num_kb_rounded += s.num_kb_rounded;
    bs.num_objects += s.num_objects;
  }

  cache->async_refresh_response(user, bucket, bs);
}

class RGWBucketStatsCache : public RGWQuotaCache<rgw_bucket> {
protected:
  bool map_find(const string& user, rgw_bucket& bucket, RGWQuotaCacheStats& qs) {
    return stats_map.find(bucket, qs);
  }

  bool map_find_and_update(const string& user, rgw_bucket& bucket, lru_map<rgw_bucket, RGWQuotaCacheStats>::UpdateContext *ctx) {
    return stats_map.find_and_update(bucket, NULL, ctx);
  }

  void map_add(const string& user, rgw_bucket& bucket, RGWQuotaCacheStats& qs) {
    stats_map.add(bucket, qs);
  }

  int fetch_stats_from_storage(const string& user, rgw_bucket& bucket, RGWStorageStats& stats);

public:
  RGWBucketStatsCache(RGWRados *_store) : RGWQuotaCache<rgw_bucket>(_store, _store->ctx()->_conf->rgw_bucket_quota_cache_size) {
  }

  AsyncRefreshHandler *allocate_refresh_handler(const string& user, rgw_bucket& bucket) {
    return new BucketAsyncRefreshHandler(store, this, user, bucket);
  }
};

int RGWBucketStatsCache::fetch_stats_from_storage(const string& user, rgw_bucket& bucket, RGWStorageStats& stats)
{
  RGWBucketInfo bucket_info;

  string bucket_ver;
  string master_ver;

  map<RGWObjCategory, RGWStorageStats> bucket_stats;
  int r = store->get_bucket_stats(bucket, &bucket_ver, &master_ver, bucket_stats, NULL);
  if (r < 0) {
    ldout(store->ctx(), 0) << "could not get bucket info for bucket=" << bucket.name << dendl;
    return r;
  }

  stats = RGWStorageStats();

  map<RGWObjCategory, RGWStorageStats>::iterator iter;
  for (iter = bucket_stats.begin(); iter != bucket_stats.end(); ++iter) {
    RGWStorageStats& s = iter->second;
    stats.num_kb += s.num_kb;
    stats.num_kb_rounded += s.num_kb_rounded;
    stats.num_objects += s.num_objects;
  }

  return 0;
}

class UserAsyncRefreshHandler : public RGWQuotaCache<string>::AsyncRefreshHandler,
                                public RGWGetUserStats_CB {
  rgw_bucket bucket;
public:
  UserAsyncRefreshHandler(RGWRados *_store, RGWQuotaCache<string> *_cache,
                          const string& _user, rgw_bucket& _bucket) :
                          RGWQuotaCache<string>::AsyncRefreshHandler(_store, _cache),
                          RGWGetUserStats_CB(_user),
                          bucket(_bucket) {}

  void drop_reference() { put(); }
  int init_fetch();
  void handle_response(int r);
};

int UserAsyncRefreshHandler::init_fetch()
{
  ldout(store->ctx(), 20) << "initiating async quota refresh for user=" << user << dendl;
  int r = store->get_user_stats_async(user, this);
  if (r < 0) {
    ldout(store->ctx(), 0) << "could not get bucket info for user=" << user << dendl;

    /* get_bucket_stats_async() dropped our reference already */
    return r;
  }

  return 0;
}

void UserAsyncRefreshHandler::handle_response(int r)
{
  if (r < 0) {
    ldout(store->ctx(), 20) << "AsyncRefreshHandler::handle_response() r=" << r << dendl;
    return; /* nothing to do here */
  }

  cache->async_refresh_response(user, bucket, stats);
}

class RGWUserStatsCache : public RGWQuotaCache<string> {
  atomic_t down_flag;
  RWLock rwlock;
  map<rgw_bucket, string> modified_buckets;

  /* thread, sync recent modified buckets info */
  class BucketsSyncThread : public Thread {
    CephContext *cct;
    RGWUserStatsCache *stats;

    Mutex lock;
    Cond cond;
  public:

    BucketsSyncThread(CephContext *_cct, RGWUserStatsCache *_s) : cct(_cct), stats(_s), lock("RGWUserStatsCache::BucketsSyncThread") {}

    void *entry() {
      ldout(cct, 20) << "BucketsSyncThread: start" << dendl;
      do {
        map<rgw_bucket, string> buckets;

        stats->swap_modified_buckets(buckets);

        for (map<rgw_bucket, string>::iterator iter = buckets.begin(); iter != buckets.end(); ++iter) {
          rgw_bucket bucket = iter->first;
          string& user = iter->second;
          ldout(cct, 20) << "BucketsSyncThread: sync user=" << user << " bucket=" << bucket << dendl;
          int r = stats->sync_bucket(user, bucket);
          if (r < 0) {
            ldout(cct, 0) << "WARNING: sync_bucket() returned r=" << r << dendl;
          }
        }

        if (stats->going_down())
          break;

        lock.Lock();
        cond.WaitInterval(cct, lock, utime_t(cct->_conf->rgw_user_quota_bucket_sync_interval, 0));
        lock.Unlock();
      } while (!stats->going_down());
      ldout(cct, 20) << "BucketsSyncThread: done" << dendl;

      return NULL;
    }

    void stop() {
      Mutex::Locker l(lock);
      cond.Signal();
    }
  };

  /*
   * thread, full sync all users stats periodically
   *
   * only sync non idle users or ones that never got synced before, this is needed so that
   * users that didn't have quota turned on before (or existed before the user objclass
   * tracked stats) need to get their backend stats up to date.
   */
  class UserSyncThread : public Thread {
    CephContext *cct;
    RGWUserStatsCache *stats;

    Mutex lock;
    Cond cond;
  public:

    UserSyncThread(CephContext *_cct, RGWUserStatsCache *_s) : cct(_cct), stats(_s), lock("RGWUserStatsCache::UserSyncThread") {}

    void *entry() {
      ldout(cct, 20) << "UserSyncThread: start" << dendl;
      do {
        int ret = stats->sync_all_users();
        if (ret < 0) {
          ldout(cct, 0) << "ERROR: sync_all_users() returned ret=" << ret << dendl;
        }

        lock.Lock();
        cond.WaitInterval(cct, lock, utime_t(cct->_conf->rgw_user_quota_sync_interval, 0));
        lock.Unlock();
      } while (!stats->going_down());
      ldout(cct, 20) << "UserSyncThread: done" << dendl;

      return NULL;
    }

    void stop() {
      Mutex::Locker l(lock);
      cond.Signal();
    }
  };

  BucketsSyncThread *buckets_sync_thread;
  UserSyncThread *user_sync_thread;
protected:
  bool map_find(const string& user, rgw_bucket& bucket, RGWQuotaCacheStats& qs) {
    return stats_map.find(user, qs);
  }

  bool map_find_and_update(const string& user, rgw_bucket& bucket, lru_map<string, RGWQuotaCacheStats>::UpdateContext *ctx) {
    return stats_map.find_and_update(user, NULL, ctx);
  }

  void map_add(const string& user, rgw_bucket& bucket, RGWQuotaCacheStats& qs) {
    stats_map.add(user, qs);
  }

  int fetch_stats_from_storage(const string& user, rgw_bucket& bucket, RGWStorageStats& stats);
  int sync_bucket(const string& user, rgw_bucket& bucket);
  int sync_user(const string& user);
  int sync_all_users();

  void data_modified(const string& user, rgw_bucket& bucket);

  void swap_modified_buckets(map<rgw_bucket, string>& out) {
    rwlock.get_write();
    modified_buckets.swap(out);
    rwlock.unlock();
  }

  template<class T> /* easier doing it as a template, Thread doesn't have ->stop() */
  void stop_thread(T **pthr) {
    T *thread = *pthr;
    if (!thread)
      return;

    thread->stop();
    thread->join();
    delete thread;
    *pthr = NULL;
  }

public:
  RGWUserStatsCache(RGWRados *_store, bool quota_threads) : RGWQuotaCache<string>(_store, _store->ctx()->_conf->rgw_bucket_quota_cache_size),
                                        rwlock("RGWUserStatsCache::rwlock") {
    if (quota_threads) {
      buckets_sync_thread = new BucketsSyncThread(store->ctx(), this);
      buckets_sync_thread->create();
      user_sync_thread = new UserSyncThread(store->ctx(), this);
      user_sync_thread->create();
    } else {
      buckets_sync_thread = NULL;
      user_sync_thread = NULL;
    }
  }
  ~RGWUserStatsCache() {
    stop();
  }

  AsyncRefreshHandler *allocate_refresh_handler(const string& user, rgw_bucket& bucket) {
    return new UserAsyncRefreshHandler(store, this, user, bucket);
  }

  bool can_use_cached_stats(RGWQuotaInfo& quota, RGWStorageStats& stats) {
    /* in the user case, the cached stats may contain a better estimation of the totals, as
     * the backend is only periodically getting updated.
     */
    return true;
  }

  bool going_down() {
    return (down_flag.read() != 0);
  }

  void stop() {
    down_flag.set(1);
    rwlock.get_write();
    stop_thread(&buckets_sync_thread);
    rwlock.unlock();
    stop_thread(&user_sync_thread);
  }
};

int RGWUserStatsCache::fetch_stats_from_storage(const string& user, rgw_bucket& bucket, RGWStorageStats& stats)
{
  int r = store->get_user_stats(user, stats);
  if (r < 0) {
    ldout(store->ctx(), 0) << "could not get user stats for user=" << user << dendl;
    return r;
  }

  return 0;
}

int RGWUserStatsCache::sync_bucket(const string& user, rgw_bucket& bucket)
{
  int r = rgw_bucket_sync_user_stats(store, user, bucket);
  if (r < 0) {
    ldout(store->ctx(), 0) << "ERROR: rgw_bucket_sync_user_stats() for user=" << user << ", bucket=" << bucket << " returned " << r << dendl;
    return r;
  }

  return 0;
}

int RGWUserStatsCache::sync_user(const string& user)
{
  cls_user_header header;
  int ret = store->cls_user_get_header(user, &header);
  if (ret < 0) {
    ldout(store->ctx(), 0) << "ERROR: can't read user header: ret=" << ret << dendl;
    return ret;
  }

  if (!store->ctx()->_conf->rgw_user_quota_sync_idle_users &&
      header.last_stats_update < header.last_stats_sync) {
    ldout(store->ctx(), 20) << "user is idle, not doing a full sync (user=" << user << ")" << dendl;
    return 0;
  }

  utime_t when_need_full_sync = header.last_stats_sync;
  when_need_full_sync += store->ctx()->_conf->rgw_user_quota_sync_wait_time;
  
  // check if enough time passed since last full sync

  ret = rgw_user_sync_all_stats(store, user);
  if (ret < 0) {
    ldout(store->ctx(), 0) << "ERROR: failed user stats sync, ret=" << ret << dendl;
    return ret;
  }

  return 0;
}

int RGWUserStatsCache::sync_all_users()
{
  string key = "user";
  void *handle;

  int ret = store->meta_mgr->list_keys_init(key, &handle);
  if (ret < 0) {
    ldout(store->ctx(), 0) << "ERROR: can't get key: ret=" << ret << dendl;
    return ret;
  }

  bool truncated;
  int max = 1000;

  do {
    list<string> keys;
    ret = store->meta_mgr->list_keys_next(handle, max, keys, &truncated);
    if (ret < 0) {
      ldout(store->ctx(), 0) << "ERROR: lists_keys_next(): ret=" << ret << dendl;
      goto done;
    }
    for (list<string>::iterator iter = keys.begin();
         iter != keys.end() && !going_down(); 
         ++iter) {
      string& user = *iter;
      ldout(store->ctx(), 20) << "RGWUserStatsCache: sync user=" << user << dendl;
      int ret = sync_user(user);
      if (ret < 0) {
        ldout(store->ctx(), 0) << "ERROR: sync_user() failed, user=" << user << " ret=" << ret << dendl;

        /* continuing to next user */
        continue;
      }
    }
  } while (truncated);

  ret = 0;
done:
  store->meta_mgr->list_keys_complete(handle);
  return ret;
}

void RGWUserStatsCache::data_modified(const string& user, rgw_bucket& bucket)
{
  /* racy, but it's ok */
  rwlock.get_read();
  bool need_update = modified_buckets.find(bucket) == modified_buckets.end();
  rwlock.unlock();

  if (need_update) {
    rwlock.get_write();
    modified_buckets[bucket] = user;
    rwlock.unlock();
  }
}


class RGWQuotaHandlerImpl : public RGWQuotaHandler {
  RGWRados *store;
  RGWBucketStatsCache bucket_stats_cache;
  RGWUserStatsCache user_stats_cache;

  int check_quota(const char *entity, RGWQuotaInfo& quota, RGWStorageStats& stats,
                  uint64_t num_objs, uint64_t size_kb) {
    ldout(store->ctx(), 20) << entity << " quota: max_objects=" << quota.max_objects
                            << " max_size_kb=" << quota.max_size_kb << dendl;

    if (quota.max_objects >= 0 &&
        stats.num_objects + num_objs > (uint64_t)quota.max_objects) {
      ldout(store->ctx(), 10) << "quota exceeded: stats.num_objects=" << stats.num_objects
                              << " " << entity << "_quota.max_objects=" << quota.max_objects << dendl;

      return -ERR_QUOTA_EXCEEDED;
    }
    if (quota.max_size_kb >= 0 &&
               stats.num_kb_rounded + size_kb > (uint64_t)quota.max_size_kb) {
      ldout(store->ctx(), 10) << "quota exceeded: stats.num_kb_rounded=" << stats.num_kb_rounded << " size_kb=" << size_kb
                              << " " << entity << "_quota.max_size_kb=" << quota.max_size_kb << dendl;
      return -ERR_QUOTA_EXCEEDED;
    }

    return 0;
  }
public:
  RGWQuotaHandlerImpl(RGWRados *_store, bool quota_threads) : store(_store), bucket_stats_cache(_store), user_stats_cache(_store, quota_threads) {}
  virtual int check_quota(const string& user, rgw_bucket& bucket,
                          RGWQuotaInfo& user_quota, RGWQuotaInfo& bucket_quota,
			  uint64_t num_objs, uint64_t size) {

    if (!bucket_quota.enabled && !user_quota.enabled)
      return 0;

    uint64_t size_kb = rgw_rounded_objsize_kb(size);

    RGWStorageStats bucket_stats;

    /*
     * we need to fetch bucket stats if the user quota is enabled, because the whole system relies
     * on us periodically updating the user's bucket stats in the user's header, this happens in
     * get_stats() if we actually fetch that info and not rely on cached data
     */

    int ret = bucket_stats_cache.get_stats(user, bucket, bucket_stats, bucket_quota);
    if (ret < 0)
      return ret;

    if (bucket_quota.enabled) {
      ret = check_quota("bucket", bucket_quota, bucket_stats, num_objs, size_kb);
      if (ret < 0)
        return ret;
    }

    if (user_quota.enabled) {
      RGWStorageStats user_stats;

      ret = user_stats_cache.get_stats(user, bucket, user_stats, user_quota);
      if (ret < 0)
        return ret;

      ret = check_quota("user", user_quota, user_stats, num_objs, size_kb);
      if (ret < 0)
        return ret;
    }

    return 0;
  }

  virtual void update_stats(const string& user, rgw_bucket& bucket, int obj_delta, uint64_t added_bytes, uint64_t removed_bytes) {
    bucket_stats_cache.adjust_stats(user, bucket, obj_delta, added_bytes, removed_bytes);
    user_stats_cache.adjust_stats(user, bucket, obj_delta, added_bytes, removed_bytes);
  }
};


RGWQuotaHandler *RGWQuotaHandler::generate_handler(RGWRados *store, bool quota_threads)
{
  return new RGWQuotaHandlerImpl(store, quota_threads);
}

void RGWQuotaHandler::free_handler(RGWQuotaHandler *handler)
{
  delete handler;
}
