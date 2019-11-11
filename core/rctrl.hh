#pragma once

#include "./qps/factory.hh"
#include "./rmem/factory.hh"

#include "./bootstrap/srpc.hh"

#include <atomic>

#include <pthread.h>

namespace rdmaio {

/*!
  RCtrl is a control path daemon, that handles all RDMA bootstrap to this
  machine. RCtrl is **thread-safe**.
 */
class RCtrl {

  std::atomic<bool> running;

  pthread_t handler_tid;

  /*!
    The two factory which allow user to **register** the QP, MR so that others
    can establish communication with them.
   */
public:
  rmem::RegFactory registered_mrs;
  qp::Factory registered_qps;
  NicFactory  opened_nics;

  bootstrap::SRpcHandler rpc;

public:
  explicit RCtrl(const usize &port) : running(false), rpc(port) {
    RDMA_ASSERT(rpc.register_handler(
        proto::FetchMr,
        std::bind(&RCtrl::fetch_mr_handler, this, std::placeholders::_1)));

    RDMA_ASSERT(rpc.register_handler(
        proto::CreateRC,
        std::bind(&RCtrl::rc_handler, this, std::placeholders::_1)));
  }

  /*!
    Start the daemon thread for handling RDMA connection requests
   */
  bool start_daemon() {
    running = true;
    asm volatile("" ::: "memory");

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_create(&handler_tid, &attr, &RCtrl::daemon, this);
  }

  /*!
    Stop the daemon thread for handling RDMA connection requests
   */
  void stop_daemon() {
    if (running) {
      running = false;

      asm volatile("" ::: "memory");
      pthread_join(handler_tid, nullptr);
    }
  }

  static void *daemon(void *ctx) {
    RCtrl &ctrl = *((RCtrl *)ctx);
    u64 total_reqs = 0;
    while (ctrl.running) {
      total_reqs +=  ctrl.rpc.run_one_event_loop();
      continue;
    }
    RDMA_LOG(INFO) << "stop with :" << total_reqs << " processed.";
  }

  // handlers of the dameon call
private:

  ByteBuffer fetch_mr_handler(const ByteBuffer &b) {
    auto o_id = ::rdmaio::Marshal::dedump<proto::MRReq>(b);
    if (o_id) {
      auto req_id = o_id.value();
      auto o_attr = registered_mrs.get_attr_byid(req_id.id);
      if (o_attr) {
        return ::rdmaio::Marshal::dump<proto::MRReply>(
            {.status = proto::CallbackStatus::Ok, .attr = o_attr.value()});

      } else {
        return ::rdmaio::Marshal::dump<proto::MRReply>(
            {.status = proto::CallbackStatus::NotFound});
      }
    }
    return ::rdmaio::Marshal::dump<proto::MRReply>(
        {.status = proto::CallbackStatus::WrongArg});
  }

  /*!
    Given a RCReq, query its attribute from the QPs
    \ret: Marshalling RCReply to a Bytebuffer
   */
  ByteBuffer fetch_qp_attr(const proto::RCReq &req) {
    auto rc = registered_qps.query_rc(req.id);
    if (rc) {
      return ::rdmaio::Marshal::dump<proto::RCReply>(
          {.status = proto::CallbackStatus::Ok, .attr = rc.value()->my_attr()});
    }
    return ::rdmaio::Marshal::dump<proto::RCReply>(
        {.status = proto::CallbackStatus::NotFound});
  }

  /*!
    Handling the RC request
    The process has two steps:
    1. check whether user wants to create a QP
    2. if so, create it using the provided parameters
    3. query the RC attribute and returns to the user
   */
  ByteBuffer rc_handler(const ByteBuffer &b) {

    auto rc_req_o = ::rdmaio::Marshal::dedump<proto::RCReq>(b);
    if (!rc_req_o)
      goto WA;
    {
      auto rc_req = rc_req_o.value();

      // 1. sanity check the request
      if (!(rc_req.whether_create == static_cast<u8>(1) ||
            rc_req.whether_create != static_cast<u8>(0)))
        goto WA;

      // 1. check whether we need to create the QP
      if (rc_req.whether_create == 1) {
        // 1.0 find the Nic to create this QP
        auto nic = opened_nics.find_opened_nic(rc_req.nic_id);
        if (!nic)
          goto WA; // failed to find Nic

        // 1.1 try to create and register this QP
        auto rc_status = registered_qps.create_and_register_rc(
            rc_req.id, nic.value(), rc_req.config);
        if (rc_status != IOCode::Ok) {
          // clean up
          goto WA;
        }

        // 1.2 finally we connect the QP
        if (rc_status.desc->connect(rc_req.attr) != IOCode::Ok) {
          // in connect error
          registered_qps.deregister_rc(rc_req.id);
          goto WA;
        }
      }

      // 2. fetch the QP result
      return fetch_qp_attr(rc_req);
    }
    // Error handling cases:
  WA: // wrong arg
    return ::rdmaio::Marshal::dump<proto::RCReply>(
        {.status = proto::CallbackStatus::WrongArg});
  }
};

} // namespace rdmaio