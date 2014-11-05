// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
#ifndef CEPH_LIBRBD_IMAGE_WATCHER_H
#define CEPH_LIBRBD_IMAGE_WATCHER_H

#include "common/Cond.h"
#include "common/Mutex.h"
#include "common/RWLock.h"
#include "include/rados/librados.hpp"
#include <string>
#include <utility>
#include <vector>
#include <boost/function.hpp>
#include "include/assert.h"

class entity_name_t;
class Context;
class Finisher;

namespace librbd {

  class AioCompletion;
  class ImageCtx;
  class ProgressContext;

  struct RemoteAsyncRequest {
    uint64_t gid;
    uint64_t handle;
    uint64_t request_id;

    RemoteAsyncRequest() : gid(), handle(), request_id() {}
    RemoteAsyncRequest(uint64_t gid_, uint64_t handle_, uint64_t request_id_)
      : gid(gid_), handle(handle_), request_id(request_id_) {}
  };

  class ImageWatcher {
  public:


    ImageWatcher(ImageCtx& image_ctx);
    ~ImageWatcher();

    bool is_lock_supported() const;
    bool is_lock_owner() const;

    int register_watch();
    int unregister_watch();

    bool has_pending_aio_operations();
    void flush_aio_operations();

    int try_lock();
    int request_lock(const boost::function<int(AioCompletion*)>& restart_op,
		     AioCompletion* c);
    int unlock();

    int notify_async_progress(const RemoteAsyncRequest &remote_async_request,
			      uint64_t offset, uint64_t total);
    int notify_async_complete(const RemoteAsyncRequest &remote_async_request,
			      int r);

    int notify_flatten(ProgressContext &prog_ctx);
    int notify_resize(uint64_t size, ProgressContext &prog_ctx);
    int notify_snap_create(const std::string &snap_name);

    static void notify_header_update(librados::IoCtx &io_ctx,
				     const std::string &oid);

  private:

    typedef std::pair<Context *, ProgressContext *> AsyncRequest;
    typedef std::pair<boost::function<int(AioCompletion *)>,
		      AioCompletion *> AioRequest;

    struct WatchCtx : public librados::WatchCtx2 {
      ImageWatcher &image_watcher;

      WatchCtx(ImageWatcher &parent) : image_watcher(parent) {}

      virtual void handle_notify(uint64_t notify_id,
                                 uint64_t handle,
				 uint64_t notifier_id,
                                 bufferlist& bl);
      virtual void handle_error(uint64_t cookie, int err);
    };

    ImageCtx &m_image_ctx;

    WatchCtx m_watch_ctx;
    uint64_t m_handle;

    bool m_lock_owner;

    Finisher *m_finisher;

    RWLock m_async_request_lock;
    uint64_t m_async_request_id;
    std::map<uint64_t, AsyncRequest> m_async_requests;

    Mutex m_aio_request_lock;
    Cond m_aio_request_cond;
    std::vector<AioRequest> m_aio_requests;
    bool m_aio_requests_pending;

    std::string encode_lock_cookie() const;
    static bool decode_lock_cookie(const std::string &cookie, uint64_t *handle);

    int get_lock_owner_info(entity_name_t *locker, std::string *cookie,
			    std::string *address, uint64_t *handle);
    int lock();
    void release_lock();
    bool try_request_lock();
    void finalize_request_lock();
    void finalize_header_update();

    void retry_aio_requests();
    void cancel_aio_requests(int result);
    void cancel_async_requests(int result);

    uint64_t encode_async_request(bufferlist &bl);
    static int decode_response_code(bufferlist &bl);

    void notify_request_lock();
    int notify_lock_owner(bufferlist &bl, bufferlist &response);

    int notify_async_request(uint64_t async_request_id, bufferlist &in,
			     ProgressContext& prog_ctx);
    void notify_request_leadership();
    int notify_leader(bufferlist &bl, bufferlist &response);

    void handle_header_update();
    void handle_acquired_lock();
    void handle_released_lock();
    void handle_request_lock(bufferlist *out);

    void handle_async_progress(bufferlist::iterator iter);
    void handle_async_complete(bufferlist::iterator iter);
    void handle_flatten(bufferlist::iterator iter, bufferlist *out);
    void handle_resize(bufferlist::iterator iter, bufferlist *out);
    void handle_snap_create(bufferlist::iterator iter, bufferlist *out);
    void handle_unknown_op(bufferlist *out);
    void handle_notify(uint64_t notify_id, uint64_t handle, bufferlist &bl);
    void acknowledge_notify(uint64_t notify_id, uint64_t handle,
			    bufferlist &out);
  };

} // namespace librbd

#endif // CEPH_LIBRBD_IMAGE_WATCHER_H
