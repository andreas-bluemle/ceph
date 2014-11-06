// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
#include "librbd/ImageWatcher.h"
#include "librbd/AioCompletion.h"
#include "librbd/ImageCtx.h"
#include "cls/lock/cls_lock_client.h"
#include "cls/lock/cls_lock_types.h"
#include "include/encoding.h"
#include "include/stringify.h"
#include "common/errno.h"
#include <sstream>
#include <boost/bind.hpp>
#include <boost/function.hpp>
#include <boost/scope_exit.hpp>

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::ImageWatcher: "

static void decode(librbd::RemoteAsyncRequest &request,
		   bufferlist::iterator &iter) {
  ::decode(request.gid, iter);
  ::decode(request.handle, iter);
  ::decode(request.request_id, iter);
}

static void encode(const librbd::RemoteAsyncRequest &request, bufferlist &bl) {
  ::encode(request.gid, bl);
  ::encode(request.handle, bl);
  ::encode(request.request_id, bl);
}

static std::ostream &operator<<(std::ostream &out,
				const librbd::RemoteAsyncRequest &request) {
  out << "[" << request.gid << "," << request.handle << ","
      << request.request_id << "]";
  return out;
}

namespace librbd {

static const std::string WATCHER_LOCK_TAG = "internal";
static const std::string WATCHER_LOCK_COOKIE_PREFIX = "auto";

static const uint64_t	NOTIFY_TIMEOUT = 5000;
static const uint8_t	NOTIFY_VERSION = 1;

enum {
  NOTIFY_OP_ACQUIRED_LOCK  = 0,
  NOTIFY_OP_RELEASED_LOCK  = 1,
  NOTIFY_OP_REQUEST_LOCK   = 2,
  NOTIFY_OP_HEADER_UPDATE  = 3,
  NOTIFY_OP_ASYNC_PROGRESS = 4,
  NOTIFY_OP_ASYNC_COMPLETE = 5,
  NOTIFY_OP_FLATTEN 	   = 6,
  NOTIFY_OP_RESIZE 	   = 7,
  NOTIFY_OP_SNAP_CREATE    = 8
};
class FunctionContext : public Context {
public:
  FunctionContext(const boost::function<void()> &callback)
    : m_callback(callback)
  {
  }

  virtual void finish(int r) {
    m_callback();
  }
private:
  boost::function<void()> m_callback;
};

class RemoteProgressContext : public ProgressContext {
public:
  RemoteProgressContext(ImageWatcher &image_watcher, Finisher &finisher,
			const RemoteAsyncRequest &remote_async_request)
    : m_image_watcher(image_watcher), m_finisher(finisher),
      m_remote_async_request(remote_async_request)
  {
  }

  virtual int update_progress(uint64_t offset, uint64_t total) {
    // TODO: JD throttle notify updates(?)
    FunctionContext *ctx = new FunctionContext(
      boost::bind(&ImageWatcher::notify_async_progress,
		  &m_image_watcher, m_remote_async_request, offset, total));
    m_finisher.queue(ctx);
    return 0;
  }

private:
  ImageWatcher &m_image_watcher;
  Finisher &m_finisher;
  RemoteAsyncRequest m_remote_async_request;
};

class RemoteContext : public Context {
public:
  RemoteContext(ImageWatcher &image_watcher, Finisher &finisher,
		const RemoteAsyncRequest &remote_async_request,
		RemoteProgressContext *prog_ctx)
    : m_image_watcher(image_watcher), m_finisher(finisher),
      m_remote_async_request(remote_async_request), m_prog_ctx(prog_ctx)
  {
  }

  ~RemoteContext() {
    delete m_prog_ctx;
  }

  virtual void finish(int r) {
    FunctionContext *ctx = new FunctionContext(
      boost::bind(&ImageWatcher::notify_async_complete,
		  &m_image_watcher, m_remote_async_request, r));
    m_finisher.queue(ctx);
  }

private:
  ImageWatcher &m_image_watcher;
  Finisher &m_finisher;
  RemoteAsyncRequest m_remote_async_request;
  RemoteProgressContext *m_prog_ctx;
};

ImageWatcher::ImageWatcher(ImageCtx &image_ctx)
  : m_image_ctx(image_ctx), m_watch_ctx(*this), m_handle(0), m_lock_owner(false),
    m_finisher(new Finisher(image_ctx.cct)),
    m_async_request_lock("librbd::ImageWatcher::m_async_request_lock"),
    m_async_request_id(0),
    m_aio_request_lock("librbd::ImageWatcher::m_aio_request_lock"),
    m_aio_requests_pending()
{
  m_finisher->start();
}

ImageWatcher::~ImageWatcher()
{
  m_finisher->stop();
  delete m_finisher;
}

bool ImageWatcher::is_lock_supported() const {
  return ((m_image_ctx.features & RBD_FEATURE_EXCLUSIVE_LOCK) != 0 &&
	  !m_image_ctx.read_only && m_image_ctx.snap_id == CEPH_NOSNAP);
}

bool ImageWatcher::is_lock_owner() const {
  // TODO issue #8903 will address lost notification handling
  // in cases where the lock was broken
  return m_lock_owner;
}

int ImageWatcher::register_watch() {
  ldout(m_image_ctx.cct, 20) << "registering image watcher" << dendl;
  return m_image_ctx.md_ctx.watch2(m_image_ctx.header_oid, &m_handle,
				   &m_watch_ctx);
}

int ImageWatcher::unregister_watch() {
  ldout(m_image_ctx.cct, 20)  << "unregistering image watcher" << dendl;
  cancel_aio_requests(-ESHUTDOWN);
  cancel_async_requests(-ESHUTDOWN);
  return m_image_ctx.md_ctx.unwatch(m_image_ctx.header_oid, m_handle);;
}

bool ImageWatcher::has_pending_aio_operations() {
  Mutex::Locker l(m_aio_request_lock);
  return !m_aio_requests.empty();
}

void ImageWatcher::flush_aio_operations() {
  Mutex::Locker l(m_aio_request_lock);
  while (m_aio_requests_pending || !m_aio_requests.empty()) {
    ldout(m_image_ctx.cct, 20)  << "flushing aio operations: "
				<< "pending=" << m_aio_requests_pending << ","
				<< "count=" << m_aio_requests.size() << dendl;
    m_aio_request_cond.Wait(m_aio_request_lock);
  }
}

int ImageWatcher::try_lock() {
  assert(m_lock_owner == false);
  assert(m_image_ctx.owner_lock.is_wlocked());

  while (true) {
    int r = lock();
    if (r != -EBUSY) {
      return r;
    }

    // determine if the current lock holder is still alive
    entity_name_t locker;
    std::string locker_cookie;
    std::string locker_address;
    uint64_t locker_handle;
    r = get_lock_owner_info(&locker, &locker_cookie, &locker_address,
			    &locker_handle);
    if (r < 0) {
      return r;
    }
    if (locker_cookie.empty() || locker_address.empty()) {
      continue;
    }

    std::list<obj_watch_t> watchers;
    r = m_image_ctx.md_ctx.list_watchers(m_image_ctx.header_oid, &watchers);
    if (r < 0) {
      return r;
    }

    for (std::list<obj_watch_t>::iterator iter = watchers.begin();
	 iter != watchers.end(); ++iter) {
      if ((strncmp(locker_address.c_str(), iter->addr, sizeof(iter->addr)) == 0) &&
	  (locker_handle == iter->cookie)) {
	return 0;
      }
    }

    ldout(m_image_ctx.cct, 1) << "breaking exclusive lock: " << locker << dendl;
    r = rados::cls::lock::break_lock(&m_image_ctx.md_ctx, m_image_ctx.header_oid,
				     RBD_LOCK_NAME, locker_cookie, locker);
    if (r < 0 && r != -ENOENT) {
      return r;
    }
  }
  return 0;
}

int ImageWatcher::request_lock(
    const boost::function<int(AioCompletion*)>& restart_op, AioCompletion* c) {
  assert(m_lock_owner == false);
  assert(m_image_ctx.owner_lock.is_locked());

  {
    Mutex::Locker l(m_aio_request_lock);
    bool request_pending = !m_aio_requests.empty();
    ldout(m_image_ctx.cct, 10) << "queuing aio request: " << c
			       << dendl;
    m_aio_requests.push_back(std::make_pair(restart_op, c));
    if (request_pending) {
      return 0;
    }
  }

  // run notify request in finisher to avoid blocking aio path
  FunctionContext *ctx = new FunctionContext(
    boost::bind(&ImageWatcher::notify_request_lock, this));
  m_finisher->queue(ctx);
  ldout(m_image_ctx.cct, 5) << "requesting exclusive lock" << dendl;
  return 0;
}

bool ImageWatcher::try_request_lock() {
  int r = try_lock();
  if (r < 0) {
    ldout(m_image_ctx.cct, 5) << "failed to acquire exclusive lock:"
			      << cpp_strerror(r) << dendl;
    cancel_aio_requests(-EROFS);
    return true;
  }

  if (is_lock_owner()) {
    ldout(m_image_ctx.cct, 5) << "successfully acquired exclusive lock"
			      << dendl;
  } else {
    ldout(m_image_ctx.cct, 5) << "unable to acquire exclusive lock, retrying"
			      << dendl;
  }
  return is_lock_owner();
}

void ImageWatcher::finalize_request_lock() {
  {
    RWLock::WLocker l(m_image_ctx.owner_lock);
    try_request_lock();
  }
  retry_aio_requests();
}

int ImageWatcher::get_lock_owner_info(entity_name_t *locker, std::string *cookie,
				      std::string *address, uint64_t *handle) {
  std::map<rados::cls::lock::locker_id_t,
	   rados::cls::lock::locker_info_t> lockers;
  ClsLockType lock_type;
  std::string lock_tag;
  int r = rados::cls::lock::get_lock_info(&m_image_ctx.md_ctx,
					  m_image_ctx.header_oid,
					  RBD_LOCK_NAME, &lockers, &lock_type,
					  &lock_tag);
  if (r < 0) {
    return r;
  }

  if (lockers.empty()) {
    ldout(m_image_ctx.cct, 20) << "no lockers detected" << dendl;
    return 0;
  }

  if (lock_tag != WATCHER_LOCK_TAG) {
    ldout(m_image_ctx.cct, 10) << "locked by external mechanism: tag="
			       << lock_tag << dendl;
    return -EBUSY;
  }

  if (lock_type == LOCK_SHARED) {
    ldout(m_image_ctx.cct, 10) << "shared lock type detected" << dendl;
    return -EBUSY;
  }

  std::map<rados::cls::lock::locker_id_t,
           rados::cls::lock::locker_info_t>::iterator iter = lockers.begin();
  if (!decode_lock_cookie(iter->first.cookie, handle)) {
    ldout(m_image_ctx.cct, 10) << "locked by external mechanism: cookie="
			       << iter->first.cookie << dendl;
    return -EBUSY;
  }

  *locker = iter->first.locker;
  *cookie = iter->first.cookie;
  *address = stringify(iter->second.addr);
  ldout(m_image_ctx.cct, 10) << "retrieved exclusive locker: " << *locker
			     << "@" << *address << dendl;
  return 0;
}

int ImageWatcher::lock() {
  int r = rados::cls::lock::lock(&m_image_ctx.md_ctx, m_image_ctx.header_oid,
				 RBD_LOCK_NAME, LOCK_EXCLUSIVE,
				 encode_lock_cookie(), WATCHER_LOCK_TAG, "",
				 utime_t(), 0);
  if (r < 0) {
    return r;
  }

  ldout(m_image_ctx.cct, 20) << "acquired exclusive lock" << dendl;
  m_lock_owner = true;

  r = m_image_ctx.refresh_object_map();
  if (r < 0) {
    unlock();
    return r;
  }

  bufferlist bl;
  ENCODE_START(NOTIFY_VERSION, NOTIFY_VERSION, bl);
  ::encode(NOTIFY_OP_ACQUIRED_LOCK, bl);
  ENCODE_FINISH(bl);

  // send the notification when we aren't holding locks
  FunctionContext *ctx = new FunctionContext(
    boost::bind(&IoCtx::notify2, &m_image_ctx.md_ctx, m_image_ctx.header_oid,
		bl, NOTIFY_TIMEOUT, reinterpret_cast<bufferlist *>(NULL)));
  m_finisher->queue(ctx);
  return 0;
}

int ImageWatcher::unlock()
{
  assert(m_image_ctx.owner_lock.is_wlocked());
  if (!m_lock_owner) {
    return 0;
  }

  ldout(m_image_ctx.cct, 20) << "releasing exclusive lock" << dendl;
  m_lock_owner = false;
  int r = rados::cls::lock::unlock(&m_image_ctx.md_ctx, m_image_ctx.header_oid,
				   RBD_LOCK_NAME, encode_lock_cookie());
  if (r < 0 && r != -ENOENT) {
    lderr(m_image_ctx.cct) << "failed to release exclusive lock: "
			   << cpp_strerror(r) << dendl;
    return r;
  }

  bufferlist bl;
  ENCODE_START(NOTIFY_VERSION, NOTIFY_VERSION, bl);
  ::encode(NOTIFY_OP_RELEASED_LOCK, bl);
  ENCODE_FINISH(bl);
  m_image_ctx.md_ctx.notify2(m_image_ctx.header_oid, bl, NOTIFY_TIMEOUT, NULL);
  return 0;
}

void ImageWatcher::release_lock()
{
  RWLock::WLocker l(m_image_ctx.owner_lock);
  {
    RWLock::WLocker l2(m_image_ctx.md_lock);
    m_image_ctx.invalidate_cache();
  }
  m_image_ctx.data_ctx.aio_flush();

  unlock();
}

void ImageWatcher::finalize_header_update() {
  librbd::notify_change(m_image_ctx.md_ctx, m_image_ctx.header_oid,
			&m_image_ctx);
}

int ImageWatcher::notify_async_progress(const RemoteAsyncRequest &request,
					uint64_t offset, uint64_t total) {
  ldout(m_image_ctx.cct, 20) << "remote async request progress: "
			     << request << " @ " << offset
			     << "/" << total << dendl;

  bufferlist bl;
  ENCODE_START(NOTIFY_VERSION, NOTIFY_VERSION, bl);
  ::encode(NOTIFY_OP_ASYNC_PROGRESS, bl);
  ::encode(request, bl);
  ::encode(offset, bl);
  ::encode(total, bl);
  ENCODE_FINISH(bl);

  m_image_ctx.md_ctx.notify2(m_image_ctx.header_oid, bl, NOTIFY_TIMEOUT, NULL);
  return 0;
}

int ImageWatcher::notify_async_complete(const RemoteAsyncRequest &request,
					int r) {
  ldout(m_image_ctx.cct, 20) << "remote async request finished: "
			     << request << " = " << r << dendl;

  bufferlist bl;
  ENCODE_START(NOTIFY_VERSION, NOTIFY_VERSION, bl);
  ::encode(NOTIFY_OP_ASYNC_COMPLETE, bl);
  ::encode(request, bl);
  ::encode(r, bl);
  ENCODE_FINISH(bl);

  librbd::notify_change(m_image_ctx.md_ctx, m_image_ctx.header_oid,
			&m_image_ctx);
  m_image_ctx.md_ctx.notify2(m_image_ctx.header_oid, bl, NOTIFY_TIMEOUT, NULL);
  return 0;
}

int ImageWatcher::notify_flatten(ProgressContext &prog_ctx) {
  bufferlist bl;
  uint64_t async_request_id;
  ENCODE_START(NOTIFY_VERSION, NOTIFY_VERSION, bl);
  ::encode(NOTIFY_OP_FLATTEN, bl);
  async_request_id = encode_async_request(bl);
  ENCODE_FINISH(bl);

  return notify_async_request(async_request_id, bl, prog_ctx);
}

int ImageWatcher::notify_resize(uint64_t size, ProgressContext &prog_ctx) {
  bufferlist bl;
  uint64_t async_request_id;
  ENCODE_START(NOTIFY_VERSION, NOTIFY_VERSION, bl);
  ::encode(NOTIFY_OP_RESIZE, bl);
  ::encode(size, bl);
  async_request_id = encode_async_request(bl);
  ENCODE_FINISH(bl);

  return notify_async_request(async_request_id, bl, prog_ctx);
}

int ImageWatcher::notify_snap_create(const std::string &snap_name) {
  bufferlist bl;
  ENCODE_START(NOTIFY_VERSION, NOTIFY_VERSION, bl);
  ::encode(NOTIFY_OP_SNAP_CREATE, bl);
  ::encode(snap_name, bl);
  ENCODE_FINISH(bl);

  bufferlist response;
  int r = notify_lock_owner(bl, response);
  if (r < 0) {
    return r;
  }
  return decode_response_code(response);
}

void ImageWatcher::notify_header_update(librados::IoCtx &io_ctx,
				        const std::string &oid)
{
  // supports legacy (empty buffer) clients
  bufferlist bl;
  ENCODE_START(NOTIFY_VERSION, NOTIFY_VERSION, bl);
  ::encode(NOTIFY_OP_HEADER_UPDATE, bl);
  ENCODE_FINISH(bl);

  io_ctx.notify2(oid, bl, NOTIFY_TIMEOUT, NULL);
}

std::string ImageWatcher::encode_lock_cookie() const {
  std::ostringstream ss;
  ss << WATCHER_LOCK_COOKIE_PREFIX << " " << m_handle;
  return ss.str();
}

bool ImageWatcher::decode_lock_cookie(const std::string &tag,
				      uint64_t *handle) {
  std::string prefix;
  std::istringstream ss(tag);
  if (!(ss >> prefix >> *handle) || prefix != WATCHER_LOCK_COOKIE_PREFIX) {
    return false;
  }
  return true;
}

void ImageWatcher::retry_aio_requests() {
  std::vector<AioRequest> lock_request_restarts;
  {
    Mutex::Locker l(m_aio_request_lock);
    assert(!m_aio_requests_pending);
    lock_request_restarts.swap(m_aio_requests);
    m_aio_requests_pending = true;
  }

  for (std::vector<AioRequest>::iterator iter = lock_request_restarts.begin();
       iter != lock_request_restarts.end(); ++iter) {
    ldout(m_image_ctx.cct, 10) << "retrying aio request: " << iter->second
			       << dendl;
    iter->first(iter->second);
  }

  Mutex::Locker l(m_aio_request_lock);
  m_aio_requests_pending = false;
  m_aio_request_cond.Signal();
}

void ImageWatcher::cancel_aio_requests(int result) {
  Mutex::Locker l(m_aio_request_lock);
  for (std::vector<AioRequest>::iterator iter = m_aio_requests.begin();
       iter != m_aio_requests.end(); ++iter) {
    AioCompletion *c = iter->second;
    c->get();
    c->lock.Lock();
    c->rval = result;
    c->lock.Unlock();
    c->finish_adding_requests(m_image_ctx.cct);
    c->put();
  }
  m_aio_requests.clear();
  m_aio_requests_pending = false;
  m_aio_request_cond.Signal();
}

void ImageWatcher::cancel_async_requests(int result) {
  RWLock::WLocker l(m_async_request_lock);
  for (std::map<uint64_t, AsyncRequest>::iterator iter = m_async_requests.begin();
       iter != m_async_requests.end(); ++iter) {
    iter->second.first->complete(result);
  }
  m_async_requests.clear();
}

uint64_t ImageWatcher::encode_async_request(bufferlist &bl) {
  RWLock::WLocker l(m_async_request_lock);
  ++m_async_request_id;

  RemoteAsyncRequest request(m_image_ctx.md_ctx.get_instance_id(),
			     m_handle, m_async_request_id);
  ::encode(request, bl);

  ldout(m_image_ctx.cct, 20) << "async request: " << request << dendl;
  return m_async_request_id;
}

int ImageWatcher::decode_response_code(bufferlist &bl) {
  int r;
  bufferlist::iterator iter = bl.begin();
  DECODE_START(NOTIFY_VERSION, iter);
  ::decode(r, iter);
  DECODE_FINISH(iter);
  return r;
}

void ImageWatcher::notify_request_lock() {
  bool try_lock_complete;
  {
    // try to lock now that we know we are not in a rados callback
    RWLock::WLocker l(m_image_ctx.owner_lock);
    try_lock_complete = try_request_lock();
  }
  if (try_lock_complete) {
    retry_aio_requests();
    return;
  }

  bufferlist bl;
  ENCODE_START(NOTIFY_VERSION, NOTIFY_VERSION, bl);
  ::encode(NOTIFY_OP_REQUEST_LOCK, bl);
  ENCODE_FINISH(bl);

  bufferlist response;
  int r = notify_lock_owner(bl, response);
  if (r == -ETIMEDOUT) {
    ldout(m_image_ctx.cct, 5) << "timed out requesting lock: retrying" << dendl;
    retry_aio_requests();
  } else if (r < 0) {
    lderr(m_image_ctx.cct) << "error requesting lock: " << cpp_strerror(r)
			   << dendl;
    cancel_aio_requests(-EROFS);
  }
}

int ImageWatcher::notify_lock_owner(bufferlist &bl, bufferlist& response) {
  bufferlist response_bl;
  int r = m_image_ctx.md_ctx.notify2(m_image_ctx.header_oid, bl, NOTIFY_TIMEOUT,
				     &response_bl);
  if (r < 0 && r != -ETIMEDOUT) {
    lderr(m_image_ctx.cct) << "lock owner notification failed: "
			   << cpp_strerror(r) << dendl;
    return r;
  }

  std::multimap<uint64_t, bufferlist> responses;
  if (response_bl.length() > 0) {
    try {
      bufferlist::iterator iter = response_bl.begin();
      ::decode(responses, iter);
    } catch (const buffer::error &err) {
      lderr(m_image_ctx.cct) << "failed to decode response" << dendl;
      return -EINVAL;
    }
  }

  bool lock_owner_responded = false;
  for (std::multimap<uint64_t, bufferlist>::iterator i = responses.begin();
       i != responses.end(); ++i) {
    if (i->second.length() > 0) {
      if (lock_owner_responded) {
	lderr(m_image_ctx.cct) << "duplicate lock owners detected" << dendl;
	return -EIO;
      }
      lock_owner_responded = true;
      response.claim(i->second);
    }
  }

  if (!lock_owner_responded) {
    lderr(m_image_ctx.cct) << "no lock owners detected" << dendl;
    return -ETIMEDOUT;
  }
  return 0;
}

int ImageWatcher::notify_async_request(uint64_t async_request_id,
				       bufferlist &in,
				       ProgressContext& prog_ctx) {
  Mutex my_lock("librbd::ImageWatcher::notify_async_request::my_lock");
  Cond cond;
  bool done = false;
  int r;
  Context *ctx = new C_SafeCond(&my_lock, &cond, &done, &r);

  {
    RWLock::WLocker l(m_async_request_lock);
    m_async_requests[async_request_id] = AsyncRequest(ctx, &prog_ctx);
  }

  BOOST_SCOPE_EXIT( (ctx)(async_request_id)(&m_async_requests)
		    (&m_async_request_lock)(&done) ) {
    RWLock::WLocker l(m_async_request_lock);
    m_async_requests.erase(async_request_id);
    if (!done) {
      delete ctx;
    }
  } BOOST_SCOPE_EXIT_END

  bufferlist response;
  r = notify_lock_owner(in, response);
  if (r < 0) {
    return r;
  }

  my_lock.Lock();
  while (!done) {
    cond.Wait(my_lock);
  }
  my_lock.Unlock();
  return r;
}

void ImageWatcher::handle_header_update() {
  ldout(m_image_ctx.cct, 1) << "image header updated" << dendl;

  Mutex::Locker lictx(m_image_ctx.refresh_lock);
  ++m_image_ctx.refresh_seq;
  m_image_ctx.perfcounter->inc(l_librbd_notify);
}

void ImageWatcher::handle_acquired_lock() {
  ldout(m_image_ctx.cct, 1) << "image exclusively locked announcement" << dendl;
  FunctionContext *ctx = new FunctionContext(
    boost::bind(&ImageWatcher::cancel_async_requests, this, -ERESTART));
  m_finisher->queue(ctx);
}

void ImageWatcher::handle_released_lock() {
  ldout(m_image_ctx.cct, 20) << "exclusive lock released" << dendl;

  Mutex::Locker l(m_aio_request_lock);
  if (!m_aio_requests.empty()) {
    ldout(m_image_ctx.cct, 20) << "queuing lock request" << dendl;
    FunctionContext *ctx = new FunctionContext(
      boost::bind(&ImageWatcher::finalize_request_lock, this));
    m_finisher->queue(ctx);
  }
}

void ImageWatcher::handle_request_lock(bufferlist *out) {
  if (is_lock_owner()) {
    // need to send something back so the client can detect a missing leader
    ENCODE_START(NOTIFY_VERSION, NOTIFY_VERSION, *out);
    ::encode(0, *out);
    ENCODE_FINISH(*out);

    ldout(m_image_ctx.cct, 5) << "exclusive lock requested, releasing" << dendl;
    FunctionContext *ctx = new FunctionContext(
      boost::bind(&ImageWatcher::release_lock, this));
    m_finisher->queue(ctx);
  }
}

void ImageWatcher::handle_async_progress(bufferlist::iterator iter) {
  RemoteAsyncRequest request;
  ::decode(request, iter);

  uint64_t offset;
  uint64_t total;
  ::decode(offset, iter);
  ::decode(total, iter);
  if (request.gid == m_image_ctx.md_ctx.get_instance_id() &&
      request.handle == m_handle) {
    RWLock::RLocker l(m_async_request_lock);
    std::map<uint64_t, AsyncRequest>::iterator iter =
      m_async_requests.find(request.request_id);
    if (iter != m_async_requests.end()) {
      ldout(m_image_ctx.cct, 20) << "request progress: "
				 << request << " @ " << offset
				 << "/" << total << dendl;
      iter->second.second->update_progress(offset, total);
    }
  }
}

void ImageWatcher::handle_async_complete(bufferlist::iterator iter) {
  RemoteAsyncRequest request;
  ::decode(request, iter);

  int r;
  ::decode(r, iter);
  if (request.gid == m_image_ctx.md_ctx.get_instance_id() &&
      request.handle == m_handle) {
    Context *ctx = NULL;
    {
      RWLock::RLocker l(m_async_request_lock);
      std::map<uint64_t, AsyncRequest>::iterator iter =
        m_async_requests.find(request.request_id);
      if (iter != m_async_requests.end()) {
	ctx = iter->second.first;
      }
    }
    if (ctx != NULL) {
      ldout(m_image_ctx.cct, 20) << "request finished: "
                                 << request << " = " << r << dendl;
      ctx->complete(r);
    }
  }
}

void ImageWatcher::handle_flatten(bufferlist::iterator iter, bufferlist *out) {
  if (is_lock_owner()) {
    RemoteAsyncRequest request;
    ::decode(request, iter);

    RemoteProgressContext *prog_ctx =
      new RemoteProgressContext(*this, *m_finisher, request);
    RemoteContext *ctx = new RemoteContext(*this, *m_finisher, request,
					   prog_ctx);

    ldout(m_image_ctx.cct, 20) << "remote flatten request: " << request << dendl;

    int r = librbd::async_flatten(&m_image_ctx, ctx, *prog_ctx);
    if (r < 0) {
      delete ctx;
    }

    ENCODE_START(NOTIFY_VERSION, NOTIFY_VERSION, *out);
    ::encode(r, *out);
    ENCODE_FINISH(*out);
  }
}

void ImageWatcher::handle_resize(bufferlist::iterator iter, bufferlist *out) {
  if (is_lock_owner()) {
    uint64_t size;
    ::decode(size, iter);

    RemoteAsyncRequest request;
    ::decode(request, iter);

    RemoteProgressContext *prog_ctx =
      new RemoteProgressContext(*this, *m_finisher, request);
    RemoteContext *ctx = new RemoteContext(*this, *m_finisher, request,
					   prog_ctx);

    ldout(m_image_ctx.cct, 20) << "remote resize request: " << request
			       << " " << size << dendl;

    int r = librbd::async_resize(&m_image_ctx, ctx, size, *prog_ctx);
    if (r < 0) {
      delete ctx;
    }

    ENCODE_START(NOTIFY_VERSION, NOTIFY_VERSION, *out);
    ::encode(r, *out);
    ENCODE_FINISH(*out);
  }
}

void ImageWatcher::handle_snap_create(bufferlist::iterator iter, bufferlist *out) {
  if (is_lock_owner()) {
    std::string snap_name;
    ::decode(snap_name, iter);

    ldout(m_image_ctx.cct, 20) << "remote snap_create request: " << snap_name << dendl;

    int r = librbd::snap_create(&m_image_ctx, snap_name.c_str(), false);
    ENCODE_START(NOTIFY_VERSION, NOTIFY_VERSION, *out);
    ::encode(r, *out);
    ENCODE_FINISH(*out);

    if (r == 0) {
      // cannot notify within a notificiation
      FunctionContext *ctx = new FunctionContext(
	boost::bind(&ImageWatcher::finalize_header_update, this));
      m_finisher->queue(ctx);
    }
  }
}

void ImageWatcher::handle_unknown_op(bufferlist *out) {
  if (is_lock_owner()) {
    ENCODE_START(NOTIFY_VERSION, NOTIFY_VERSION, *out);
    ::encode(-EOPNOTSUPP, *out);
    ENCODE_FINISH(*out);
  }
}

void ImageWatcher::handle_notify(uint64_t notify_id, uint64_t handle,
				 bufferlist &bl) {
  if (bl.length() == 0) {
    // legacy notification for header updates
    bufferlist out;
    acknowledge_notify(notify_id, handle, out);
    handle_header_update();
    return;
  }

  bufferlist::iterator iter = bl.begin();
  try {
    DECODE_START(NOTIFY_VERSION, iter);
    int op;
    ::decode(op, iter);

    bufferlist out;
    switch (op) {
    // client ops
    case NOTIFY_OP_ACQUIRED_LOCK:
      acknowledge_notify(notify_id, handle, out);
      handle_acquired_lock();
      break;
    case NOTIFY_OP_RELEASED_LOCK:
      acknowledge_notify(notify_id, handle, out);
      handle_released_lock();
      break;
    case NOTIFY_OP_HEADER_UPDATE:
      acknowledge_notify(notify_id, handle, out);
      handle_header_update();
      break;
    case NOTIFY_OP_ASYNC_PROGRESS:
      acknowledge_notify(notify_id, handle, out);
      handle_async_progress(iter);
      break;
    case NOTIFY_OP_ASYNC_COMPLETE:
      acknowledge_notify(notify_id, handle, out);
      handle_async_complete(iter);
      break;

    // lock owner-only ops
    case NOTIFY_OP_REQUEST_LOCK:
      handle_request_lock(&out);
      acknowledge_notify(notify_id, handle, out);
      break;
    case NOTIFY_OP_FLATTEN:
      handle_flatten(iter, &out);
      acknowledge_notify(notify_id, handle, out);
      break;
    case NOTIFY_OP_RESIZE:
      handle_resize(iter, &out);
      acknowledge_notify(notify_id, handle, out);
      break;
    case NOTIFY_OP_SNAP_CREATE:
      handle_snap_create(iter, &out);
      acknowledge_notify(notify_id, handle, out);
      break;

    default:
      handle_unknown_op(&out);
      acknowledge_notify(notify_id, handle, out);
      break;
    }
    DECODE_FINISH(iter);
  } catch (const buffer::error &err) {
    lderr(m_image_ctx.cct) << "error decoding image notification" << dendl;
  }
}

void ImageWatcher::acknowledge_notify(uint64_t notify_id, uint64_t handle,
				      bufferlist &out) {
  m_image_ctx.md_ctx.notify_ack(m_image_ctx.header_oid, notify_id, handle, out);
}

void ImageWatcher::WatchCtx::handle_notify(uint64_t notify_id,
	         uint64_t handle, uint64_t notifier_id,
	         bufferlist& bl) {
  image_watcher.handle_notify(notify_id, handle, bl);
}

void ImageWatcher::WatchCtx::handle_error(uint64_t cookie, int err) {
}

}
