// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "common/Clock.h"
#include "common/Timer.h"
#include "common/utf8.h"
#include "common/OutputDataSocket.h"
#include "common/Formatter.h"

#include "rgw_log.h"
#include "rgw_acl.h"
#include "rgw_rados.h"
#include "rgw_client_io.h"

#define dout_subsys ceph_subsys_rgw

static void set_param_str(struct req_state *s, const char *name, string& str)
{
  const char *p = s->info.env->get(name);
  if (p)
    str = p;
}

string render_log_object_name(const string& format,
			      struct tm *dt, string& bucket_id, const string& bucket_name)
{
  string o;
  for (unsigned i=0; i<format.size(); i++) {
    if (format[i] == '%' && i+1 < format.size()) {
      i++;
      char buf[32];
      switch (format[i]) {
      case '%':
	strcpy(buf, "%");
	break;
      case 'Y':
	sprintf(buf, "%.4d", dt->tm_year + 1900);
	break;
      case 'y':
	sprintf(buf, "%.2d", dt->tm_year % 100);
	break;
      case 'm':
	sprintf(buf, "%.2d", dt->tm_mon + 1);
	break;
      case 'd':
	sprintf(buf, "%.2d", dt->tm_mday);
	break;
      case 'H':
	sprintf(buf, "%.2d", dt->tm_hour);
	break;
      case 'I':
	sprintf(buf, "%.2d", (dt->tm_hour % 12) + 1);
	break;
      case 'k':
	sprintf(buf, "%d", dt->tm_hour);
	break;
      case 'l':
	sprintf(buf, "%d", (dt->tm_hour % 12) + 1);
	break;
      case 'M':
	sprintf(buf, "%.2d", dt->tm_min);
	break;

      case 'i':
	o += bucket_id;
	continue;
      case 'n':
	o += bucket_name;
	continue;
      default:
	// unknown code
	sprintf(buf, "%%%c", format[i]);
	break;
      }
      o += buf;
      continue;
    }
    o += format[i];
  }
  return o;
}

/* usage logger */
class UsageLogger {
  CephContext *cct;
  RGWRados *store;
  map<rgw_user_bucket, RGWUsageBatch> usage_map;
  Mutex lock;
  int32_t num_entries;
  Mutex timer_lock;
  SafeTimer timer;
  utime_t round_timestamp;

  class C_UsageLogTimeout : public Context {
    UsageLogger *logger;
  public:
    C_UsageLogTimeout(UsageLogger *_l) : logger(_l) {}
    void finish(int r) {
      logger->flush();
      logger->set_timer();
    }
  };

  void set_timer() {
    timer.add_event_after(cct->_conf->rgw_usage_log_tick_interval, new C_UsageLogTimeout(this));
  }
public:

  UsageLogger(CephContext *_cct, RGWRados *_store) : cct(_cct), store(_store), lock("UsageLogger"), num_entries(0), timer_lock("UsageLogger::timer_lock"), timer(cct, timer_lock) {
    timer.init();
    Mutex::Locker l(timer_lock);
    set_timer();
    utime_t ts = ceph_clock_now(cct);
    recalc_round_timestamp(ts);
  }

  ~UsageLogger() {
    Mutex::Locker l(timer_lock);
    flush();
    timer.cancel_all_events();
    timer.shutdown();
  }

  void recalc_round_timestamp(utime_t& ts) {
    round_timestamp = ts.round_to_hour();
  }

  void insert(utime_t& timestamp, rgw_usage_log_entry& entry) {
    lock.Lock();
    if (timestamp.sec() > round_timestamp + 3600)
      recalc_round_timestamp(timestamp);
    entry.epoch = round_timestamp.sec();
    bool account;
    rgw_user_bucket ub(entry.owner, entry.bucket);
    usage_map[ub].insert(round_timestamp, entry, &account);
    if (account)
      num_entries++;
    bool need_flush = (num_entries > cct->_conf->rgw_usage_log_flush_threshold);
    lock.Unlock();
    if (need_flush) {
      Mutex::Locker l(timer_lock);
      flush();
    }
  }

  void flush() {
    map<rgw_user_bucket, RGWUsageBatch> old_map;
    lock.Lock();
    old_map.swap(usage_map);
    num_entries = 0;
    lock.Unlock();

    store->log_usage(old_map);
  }
};

static UsageLogger *usage_logger = NULL;

void rgw_log_usage_init(CephContext *cct, RGWRados *store)
{
  usage_logger = new UsageLogger(cct, store);
}

void rgw_log_usage_finalize()
{
  delete usage_logger;
  usage_logger = NULL;
}

static void log_usage(struct req_state *s, const string& op_name)
{
  if (s->system_request) /* don't log system user operations */
    return;

  if (!usage_logger)
    return;

  string user;

  if (!s->bucket_name_str.empty())
    user = s->bucket_owner.get_id();
  else
    user = s->user.user_id;

  rgw_usage_log_entry entry(user, s->bucket.name);

  uint64_t bytes_sent = s->cio->get_bytes_sent();
  uint64_t bytes_received = s->cio->get_bytes_received();

  rgw_usage_data data(bytes_sent, bytes_received);

  data.ops = 1;
  if (!s->err.is_err())
    data.successful_ops = 1;

  entry.add(op_name, data);

  utime_t ts = ceph_clock_now(s->cct);

  usage_logger->insert(ts, entry);
}

void rgw_format_ops_log_entry(struct rgw_log_entry& entry, Formatter *formatter)
{
  formatter->open_object_section("log_entry");
  formatter->dump_string("bucket", entry.bucket);
  entry.time.gmtime(formatter->dump_stream("time"));      // UTC
  entry.time.localtime(formatter->dump_stream("time_local"));
  formatter->dump_string("remote_addr", entry.remote_addr);
  if (entry.object_owner.length())
    formatter->dump_string("object_owner", entry.object_owner);
  formatter->dump_string("user", entry.user);
  formatter->dump_string("operation", entry.op);
  formatter->dump_string("uri", entry.uri);
  formatter->dump_string("http_status", entry.http_status);
  formatter->dump_string("error_code", entry.error_code);
  formatter->dump_int("bytes_sent", entry.bytes_sent);
  formatter->dump_int("bytes_received", entry.bytes_received);
  formatter->dump_int("object_size", entry.obj_size);
  uint64_t total_time =  entry.total_time.sec() * 1000000LL * entry.total_time.usec();

  formatter->dump_int("total_time", total_time);
  formatter->dump_string("user_agent",  entry.user_agent);
  formatter->dump_string("referrer",  entry.referrer);
  formatter->close_section();
}

void OpsLogSocket::formatter_to_bl(bufferlist& bl)
{
  stringstream ss;
  formatter->flush(ss);
  const string& s = ss.str();

  bl.append(s);
}

void OpsLogSocket::init_connection(bufferlist& bl)
{
  bl.append("[");
}

OpsLogSocket::OpsLogSocket(CephContext *cct, uint64_t _backlog) : OutputDataSocket(cct, _backlog), lock("OpsLogSocket")
{
  formatter = new JSONFormatter;
  delim.append(",\n");
}

OpsLogSocket::~OpsLogSocket()
{
  delete formatter;
}

void OpsLogSocket::log(struct rgw_log_entry& entry)
{
  bufferlist bl;

  lock.Lock();
  rgw_format_ops_log_entry(entry, formatter);
  formatter_to_bl(bl);
  lock.Unlock();

  append_output(bl);
}

int rgw_log_op(RGWRados *store, struct req_state *s, const string& op_name, OpsLogSocket *olog)
{
  struct rgw_log_entry entry;
  string bucket_id;

  if (s->enable_usage_log)
    log_usage(s, op_name);

  if (!s->enable_ops_log)
    return 0;

  if (s->bucket_name_str.empty()) {
    ldout(s->cct, 5) << "nothing to log for operation" << dendl;
    return -EINVAL;
  }
  if (s->err.ret == -ERR_NO_SUCH_BUCKET) {
    if (!s->cct->_conf->rgw_log_nonexistent_bucket) {
      ldout(s->cct, 5) << "bucket " << s->bucket << " doesn't exist, not logging" << dendl;
      return 0;
    }
    bucket_id = "";
  } else {
    bucket_id = s->bucket.bucket_id;
  }
  entry.bucket = s->bucket_name_str;

  if (check_utf8(s->bucket_name_str.c_str(), entry.bucket.size()) != 0) {
    ldout(s->cct, 5) << "not logging op on bucket with non-utf8 name" << dendl;
    return 0;
  }

  if (!s->object.empty()) {
    entry.obj = s->object;
  } else {
    entry.obj = rgw_obj_key("-");
  }

  entry.obj_size = s->obj_size;

  if (s->cct->_conf->rgw_remote_addr_param.length())
    set_param_str(s, s->cct->_conf->rgw_remote_addr_param.c_str(), entry.remote_addr);
  else
    set_param_str(s, "REMOTE_ADDR", entry.remote_addr);    
  set_param_str(s, "HTTP_USER_AGENT", entry.user_agent);
  set_param_str(s, "HTTP_REFERRER", entry.referrer);
  set_param_str(s, "REQUEST_URI", entry.uri);
  set_param_str(s, "REQUEST_METHOD", entry.op);

  entry.user = s->user.user_id;
  if (s->object_acl)
    entry.object_owner = s->object_acl->get_owner().get_id();
  entry.bucket_owner = s->bucket_owner.get_id();


  uint64_t bytes_sent = s->cio->get_bytes_sent();
  uint64_t bytes_received = s->cio->get_bytes_received();

  entry.time = s->time;
  entry.total_time = ceph_clock_now(s->cct) - s->time;
  entry.bytes_sent = bytes_sent;
  entry.bytes_received = bytes_received;
  if (s->err.http_ret) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", s->err.http_ret);
    entry.http_status = buf;
  } else
    entry.http_status = "200"; // default

  entry.error_code = s->err.s3_code;
  entry.bucket_id = bucket_id;

  bufferlist bl;
  ::encode(entry, bl);

  struct tm bdt;
  time_t t = entry.time.sec();
  if (s->cct->_conf->rgw_log_object_name_utc)
    gmtime_r(&t, &bdt);
  else
    localtime_r(&t, &bdt);

  int ret = 0;

  if (s->cct->_conf->rgw_ops_log_rados) {
    string oid = render_log_object_name(s->cct->_conf->rgw_log_object_name, &bdt,
				        s->bucket.bucket_id, entry.bucket);

    rgw_obj obj(store->zone.log_pool, oid);

    ret = store->append_async(obj, bl.length(), bl);
    if (ret == -ENOENT) {
      ret = store->create_pool(store->zone.log_pool);
      if (ret < 0)
        goto done;
      // retry
      ret = store->append_async(obj, bl.length(), bl);
    }
  }

  if (olog) {
    olog->log(entry);
  }
done:
  if (ret < 0)
    ldout(s->cct, 0) << "ERROR: failed to log entry" << dendl;

  return ret;
}

