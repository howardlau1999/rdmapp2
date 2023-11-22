#include "rdmapp/qp.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <endian.h>
#include <exception>
#include <functional>
#include <iterator>
#include <memory>
#include <netdb.h>
#include <netinet/in.h>
#include <optional>
#include <stdexcept>
#include <strings.h>
#include <sys/socket.h>
#include <utility>
#include <vector>

#include <infiniband/verbs.h>

#include "rdmapp/error.h"
#include "rdmapp/pd.h"
#include "rdmapp/srq.h"

#include "rdmapp/detail/serdes.h"

namespace rdmapp {

std::atomic<uint32_t> qp::next_sq_psn = 1;
qp::qp(uint16_t remote_lid, uint32_t remote_qpn, uint32_t remote_psn, pd *pd,
       cq *cq, srq *srq)
    : qp(remote_lid, remote_qpn, remote_psn, pd, cq, cq, srq) {}
qp::qp(uint16_t remote_lid, uint32_t remote_qpn, uint32_t remote_psn, pd *pd,
       cq *recv_cq, cq *send_cq, srq *srq)
    : qp(pd, recv_cq, send_cq, srq) {
  rtr(remote_lid, remote_qpn, remote_psn);
  rts();
}

qp::qp(rdmapp::pd *pd, cq *cq, srq *srq) : qp(pd, cq, cq, srq) {}

qp::qp(rdmapp::pd *pd, cq *recv_cq, cq *send_cq, srq *srq)
    : qp_(nullptr), pd_(pd), recv_cq_(recv_cq), send_cq_(send_cq), srq_(srq) {
  create();
  init();
}

std::vector<uint8_t> &qp::user_data() { return user_data_; }

pd *qp::pd_ptr() const { return pd_; }

std::vector<uint8_t> qp::serialize() const {
  std::vector<uint8_t> buffer;
  auto it = std::back_inserter(buffer);
  detail::serialize(pd_->device_ptr()->lid(), it);
  detail::serialize(qp_->qp_num, it);
  detail::serialize(sq_psn_, it);
  detail::serialize(static_cast<uint32_t>(user_data_.size()), it);
  std::copy(user_data_.cbegin(), user_data_.cend(), it);
  return buffer;
}

void qp::create() {
  struct ibv_qp_init_attr qp_init_attr = {};
  ::bzero(&qp_init_attr, sizeof(qp_init_attr));
  qp_init_attr.qp_type = IBV_QPT_RC;
  qp_init_attr.recv_cq = recv_cq_->cq_;
  qp_init_attr.send_cq = send_cq_->cq_;
  qp_init_attr.cap.max_recv_sge = 1;
  qp_init_attr.cap.max_send_sge = 1;
  qp_init_attr.cap.max_recv_wr = 128;
  qp_init_attr.cap.max_send_wr = 128;
  qp_init_attr.sq_sig_all = 0;
  qp_init_attr.qp_context = this;

  if (srq_ != nullptr) {
    qp_init_attr.srq = srq_->srq_;
    raw_srq_ = srq_->srq_;
    post_recv_fn = &qp::post_recv_srq;
  } else {
    post_recv_fn = &qp::post_recv_rq;
  }

  qp_ = ::ibv_create_qp(pd_->pd_, &qp_init_attr);
  check_ptr(qp_, "failed to create qp");
  sq_psn_ = next_sq_psn.fetch_add(1);
}

void qp::init() {
  struct ibv_qp_attr qp_attr = {};
  ::bzero(&qp_attr, sizeof(qp_attr));
  qp_attr.qp_state = IBV_QPS_INIT;
  qp_attr.pkey_index = 0;
  qp_attr.port_num = pd_->device_ptr()->port_num();
  qp_attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ |
                            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_ATOMIC;
  try {
    check_rc(::ibv_modify_qp(qp_, &(qp_attr),
                             IBV_QP_STATE | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS |
                                 IBV_QP_PKEY_INDEX),
             "failed to transition qp to init state");
  } catch (const std::exception &e) {
    destroy();
    throw;
  }
}

void qp::rtr(uint16_t remote_lid, uint32_t remote_qpn, uint32_t remote_psn) {
  struct ibv_qp_attr qp_attr = {};
  ::bzero(&qp_attr, sizeof(qp_attr));
  qp_attr.qp_state = IBV_QPS_RTR;
  qp_attr.path_mtu = IBV_MTU_4096;
  qp_attr.dest_qp_num = remote_qpn;
  qp_attr.rq_psn = remote_psn;
  qp_attr.max_dest_rd_atomic = 1;
  qp_attr.min_rnr_timer = 12;
  qp_attr.ah_attr.is_global = 0;
  qp_attr.ah_attr.dlid = remote_lid;
  qp_attr.ah_attr.sl = 0;
  qp_attr.ah_attr.src_path_bits = 0;
  qp_attr.ah_attr.port_num = pd_->device_ptr()->port_num();

  try {
    check_rc(::ibv_modify_qp(qp_, &qp_attr,
                             IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                                 IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                                 IBV_QP_MIN_RNR_TIMER |
                                 IBV_QP_MAX_DEST_RD_ATOMIC),
             "failed to transition qp to rtr state");
  } catch (const std::exception &e) {
    destroy();
    throw;
  }
}

void qp::rts() {
  struct ibv_qp_attr qp_attr = {};
  ::bzero(&qp_attr, sizeof(qp_attr));
  qp_attr.qp_state = IBV_QPS_RTS;
  qp_attr.timeout = 14;
  qp_attr.retry_cnt = 1;
  qp_attr.rnr_retry = 1;
  qp_attr.max_rd_atomic = 1;
  qp_attr.sq_psn = sq_psn_;

  try {
    check_rc(::ibv_modify_qp(qp_, &qp_attr,
                             IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                                 IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
                                 IBV_QP_MAX_QP_RD_ATOMIC),
             "failed to transition qp to rts state");
  } catch (std::exception const &e) {
    destroy();
    throw;
  }
}

void qp::post_send(struct ibv_send_wr const &send_wr,
                   struct ibv_send_wr *&bad_send_wr) {

  check_rc(::ibv_post_send(qp_, const_cast<struct ibv_send_wr *>(&send_wr),
                           &bad_send_wr),
           "failed to post send");
}

void qp::post_recv(struct ibv_recv_wr const &recv_wr,
                   struct ibv_recv_wr *&bad_recv_wr) const {
  (this->*(post_recv_fn))(recv_wr, bad_recv_wr);
}

void qp::post_recv_rq(struct ibv_recv_wr const &recv_wr,
                      struct ibv_recv_wr *&bad_recv_wr) const {
  check_rc(::ibv_post_recv(qp_, const_cast<struct ibv_recv_wr *>(&recv_wr),
                           &bad_recv_wr),
           "failed to post recv");
}

void qp::post_recv_srq(struct ibv_recv_wr const &recv_wr,
                       struct ibv_recv_wr *&bad_recv_wr) const {
  check_rc(::ibv_post_srq_recv(raw_srq_,
                               const_cast<struct ibv_recv_wr *>(&recv_wr),
                               &bad_recv_wr),
           "failed to post srq recv");
}

qp::send_awaitable::send_awaitable(qp *qp, local_mr *local_mr,
                                   enum ibv_wr_opcode opcode)
    : wc_(), qp_(qp), local_addr_(local_mr->addr()),
      local_length_(local_mr->length()), lkey_(local_mr->lkey()),
      opcode_(opcode) {}
qp::send_awaitable::send_awaitable(qp *qp, local_mr *local_mr,
                                   enum ibv_wr_opcode opcode,
                                   remote_mr const &remote_mr)
    : qp_(qp), local_addr_(local_mr->addr()), local_length_(local_mr->length()),
      lkey_(local_mr->lkey()), remote_addr_(remote_mr.addr()),
      remote_length_(remote_mr.length()), rkey_(remote_mr.rkey()),
      opcode_(opcode) {}
qp::send_awaitable::send_awaitable(qp *qp, local_mr *local_mr,
                                   enum ibv_wr_opcode opcode,
                                   remote_mr const &remote_mr, uint32_t imm)
    : qp_(qp), local_addr_(local_mr->addr()), local_length_(local_mr->length()),
      lkey_(local_mr->lkey()), remote_addr_(remote_mr.addr()),
      remote_length_(remote_mr.length()), rkey_(remote_mr.rkey()), imm_(imm),
      opcode_(opcode) {}
qp::send_awaitable::send_awaitable(qp *qp, local_mr *local_mr,
                                   enum ibv_wr_opcode opcode,
                                   remote_mr const &remote_mr, uint64_t add)
    : qp_(qp), local_addr_(local_mr->addr()), local_length_(local_mr->length()),
      lkey_(local_mr->lkey()), remote_addr_(remote_mr.addr()),
      remote_length_(remote_mr.length()), rkey_(remote_mr.rkey()),
      compare_add_(add), opcode_(opcode) {}
qp::send_awaitable::send_awaitable(qp *qp, local_mr *local_mr,
                                   enum ibv_wr_opcode opcode,
                                   remote_mr const &remote_mr, uint64_t compare,
                                   uint64_t swap)
    : qp_(qp), local_addr_(local_mr->addr()), local_length_(local_mr->length()),
      lkey_(local_mr->lkey()), remote_addr_(remote_mr.addr()),
      remote_length_(remote_mr.length()), rkey_(remote_mr.rkey()),
      compare_add_(compare), swap_(swap), opcode_(opcode) {}

qp::send_awaitable::send_awaitable(qp *qp, void *local_addr,
                                   size_t local_length, uint32_t lkey,
                                   enum ibv_wr_opcode opcode)
    : wc_(), qp_(qp), local_addr_(local_addr), local_length_(local_length),
      lkey_(lkey), opcode_(opcode) {}

qp::send_awaitable::send_awaitable(qp *qp, void *local_addr,
                                   size_t local_length, uint32_t lkey,
                                   enum ibv_wr_opcode opcode, void *remote_addr,
                                   size_t remote_length, uint32_t rkey)
    : wc_(), qp_(qp), local_addr_(local_addr), local_length_(local_length),
      lkey_(lkey), remote_addr_(remote_addr), remote_length_(remote_length),
      rkey_(rkey), opcode_(opcode) {}

qp::send_awaitable::send_awaitable(qp *qp, void *local_addr,
                                   size_t local_length, uint32_t lkey,
                                   enum ibv_wr_opcode opcode, void *remote_addr,
                                   size_t remote_length, uint32_t rkey,
                                   uint32_t imm)
    : wc_(), qp_(qp), local_addr_(local_addr), local_length_(local_length),
      lkey_(lkey), remote_addr_(remote_addr), remote_length_(remote_length),
      rkey_(rkey), imm_(imm), opcode_(opcode) {}

qp::send_awaitable::send_awaitable(qp *qp, void *local_addr,
                                   size_t local_length, uint32_t lkey,
                                   enum ibv_wr_opcode opcode, void *remote_addr,
                                   size_t remote_length, uint32_t rkey,
                                   uint64_t add)
    : wc_(), qp_(qp), local_addr_(local_addr), local_length_(local_length),
      lkey_(lkey), remote_addr_(remote_addr), remote_length_(remote_length),
      rkey_(rkey), compare_add_(add), opcode_(opcode) {}

qp::send_awaitable::send_awaitable(qp *qp, void *local_addr,
                                   size_t local_length, uint32_t lkey,
                                   enum ibv_wr_opcode opcode, void *remote_addr,
                                   size_t remote_length, uint32_t rkey,
                                   uint64_t compare, uint64_t swap)
    : wc_(), qp_(qp), local_addr_(local_addr), local_length_(local_length),
      lkey_(lkey), remote_addr_(remote_addr), remote_length_(remote_length),
      rkey_(rkey), compare_add_(compare), swap_(swap), opcode_(opcode) {}

static inline struct ibv_sge fill_local_sge(void *addr, size_t length,
                                            uint32_t lkey) {
  struct ibv_sge sge = {};
  sge.addr = reinterpret_cast<uint64_t>(addr);
  sge.length = length;
  sge.lkey = lkey;
  return sge;
}

bool qp::send_awaitable::await_ready() const noexcept { return false; }
bool qp::send_awaitable::await_suspend(std::coroutine_handle<> h) noexcept {
  coroutine_addr_ = h.address();
  auto send_sge = fill_local_sge(local_addr_, local_length_, lkey_);

  struct ibv_send_wr send_wr = {};
  struct ibv_send_wr *bad_send_wr = nullptr;
  send_wr.opcode = opcode_;
  send_wr.next = nullptr;
  send_wr.num_sge = 1;
  send_wr.wr_id = reinterpret_cast<uint64_t>(this);
  send_wr.send_flags = IBV_SEND_SIGNALED;
  send_wr.sg_list = &send_sge;
  if (is_rdma()) {
    assert(remote_mr_.addr() != nullptr);
    send_wr.wr.rdma.remote_addr = reinterpret_cast<uint64_t>(remote_addr_);
    send_wr.wr.rdma.rkey = rkey_;
    if (opcode_ == IBV_WR_RDMA_WRITE_WITH_IMM) {
      send_wr.imm_data = imm_;
    }
  } else if (is_atomic()) {
    assert(remote_mr_.addr() != nullptr);
    send_wr.wr.atomic.remote_addr = reinterpret_cast<uint64_t>(remote_addr_);
    send_wr.wr.atomic.rkey = rkey_;
    send_wr.wr.atomic.compare_add = compare_add_;
    if (opcode_ == IBV_WR_ATOMIC_CMP_AND_SWP) {
      send_wr.wr.atomic.swap = swap_;
    }
  }

  try {
    qp_->post_send(send_wr, bad_send_wr);
  } catch (std::runtime_error &e) {
    exception_ = std::make_exception_ptr(e);
    return false;
  }
  return true;
}

constexpr bool qp::send_awaitable::is_rdma() const {
  return opcode_ == IBV_WR_RDMA_READ || opcode_ == IBV_WR_RDMA_WRITE ||
         opcode_ == IBV_WR_RDMA_WRITE_WITH_IMM;
}

constexpr bool qp::send_awaitable::is_atomic() const {
  return opcode_ == IBV_WR_ATOMIC_CMP_AND_SWP ||
         opcode_ == IBV_WR_ATOMIC_FETCH_AND_ADD;
}

uint32_t qp::send_awaitable::await_resume() const {
  if (exception_) [[unlikely]] {
    std::rethrow_exception(exception_);
  }
  check_wc_status(wc_.status, "failed to send");
  return wc_.byte_len;
}

qp::send_awaitable qp::send(local_mr *local_mr) {
  return qp::send_awaitable(this, local_mr, IBV_WR_SEND);
}

qp::send_awaitable qp::send(void *local_addr, size_t local_length,
                            uint32_t lkey) {
  return qp::send_awaitable(this, local_addr, local_length, lkey, IBV_WR_SEND);
}

qp::send_awaitable qp::write(remote_mr const &remote_mr, local_mr *local_mr) {
  return qp::send_awaitable(this, local_mr, IBV_WR_RDMA_WRITE, remote_mr);
}

qp::send_awaitable qp::write(void *remote_addr, size_t remote_length,
                             uint32_t rkey, void *local_addr,
                             size_t local_length, uint32_t lkey) {
  return qp::send_awaitable(this, local_addr, local_length, lkey,
                            IBV_WR_RDMA_WRITE, remote_addr, remote_length,
                            rkey);
}

qp::send_awaitable qp::write_with_imm(remote_mr const &remote_mr,
                                      local_mr *local_mr, uint32_t imm) {
  return qp::send_awaitable(this, local_mr, IBV_WR_RDMA_WRITE_WITH_IMM,
                            remote_mr, imm);
}

qp::send_awaitable qp::write_with_imm(void *remote_addr, size_t remote_length,
                                      uint32_t rkey, void *local_addr,
                                      size_t local_length, uint32_t lkey,
                                      uint32_t imm) {
  return qp::send_awaitable(this, local_addr, local_length, lkey,
                            IBV_WR_RDMA_WRITE_WITH_IMM, remote_addr,
                            remote_length, rkey, imm);
}

qp::send_awaitable qp::read(remote_mr const &remote_mr, local_mr *local_mr) {
  return qp::send_awaitable(this, local_mr, IBV_WR_RDMA_READ, remote_mr);
}

qp::send_awaitable qp::read(void *remote_addr, size_t remote_length,
                            uint32_t rkey, void *local_addr,
                            size_t local_length, uint32_t lkey) {
  return qp::send_awaitable(this, local_addr, local_length, lkey,
                            IBV_WR_RDMA_READ, remote_addr, remote_length, rkey);
}

qp::send_awaitable qp::fetch_and_add(remote_mr const &remote_mr,
                                     local_mr *local_mr, uint64_t add) {
  assert(pd_->device_ptr()->is_fetch_and_add_supported());
  return qp::send_awaitable(this, local_mr, IBV_WR_ATOMIC_FETCH_AND_ADD,
                            remote_mr, add);
}

qp::send_awaitable qp::fetch_and_add(void *remote_addr, size_t remote_length,
                                     uint32_t rkey, void *local_addr,
                                     size_t local_length, uint32_t lkey,
                                     uint64_t add) {
  assert(pd_->device_ptr()->is_fetch_and_add_supported());
  return qp::send_awaitable(this, local_addr, local_length, lkey,
                            IBV_WR_ATOMIC_FETCH_AND_ADD, remote_addr,
                            remote_length, rkey, add);
}

qp::send_awaitable qp::compare_and_swap(remote_mr const &remote_mr,
                                        local_mr *local_mr, uint64_t compare,
                                        uint64_t swap) {
  assert(pd_->device_ptr()->is_compare_and_swap_supported());
  return qp::send_awaitable(this, local_mr, IBV_WR_ATOMIC_CMP_AND_SWP,
                            remote_mr, compare, swap);
}

qp::send_awaitable qp::compare_and_swap(void *remote_addr, size_t remote_length,
                                        uint32_t rkey, void *local_addr,
                                        size_t local_length, uint32_t lkey,
                                        uint64_t compare, uint64_t swap) {
  assert(pd_->device_ptr()->is_compare_and_swap_supported());
  return qp::send_awaitable(this, local_addr, local_length, lkey,
                            IBV_WR_ATOMIC_CMP_AND_SWP, remote_addr,
                            remote_length, rkey, compare, swap);
}

qp::recv_awaitable::recv_awaitable(qp *qp, local_mr *local_mr)
    : wc_(), qp_(qp), local_addr_(local_mr->addr()),
      local_length_(local_mr->length()), lkey_(local_mr->lkey()) {}

qp::recv_awaitable::recv_awaitable(qp *qp, void *local_addr,
                                   size_t local_length, uint32_t lkey)
    : wc_(), qp_(qp), local_addr_(local_addr), local_length_(local_length),
      lkey_(lkey) {}

bool qp::recv_awaitable::await_ready() const noexcept { return false; }
bool qp::recv_awaitable::await_suspend(std::coroutine_handle<> h) noexcept {
  coroutine_addr_ = h.address();
  auto recv_sge = fill_local_sge(local_addr_, local_length_, lkey_);

  struct ibv_recv_wr recv_wr = {};
  struct ibv_recv_wr *bad_recv_wr = nullptr;
  recv_wr.next = nullptr;
  recv_wr.num_sge = 1;
  recv_wr.wr_id = reinterpret_cast<uint64_t>(this);
  recv_wr.sg_list = &recv_sge;

  try {
    qp_->post_recv(recv_wr, bad_recv_wr);
  } catch (std::runtime_error &e) {
    exception_ = std::make_exception_ptr(e);
    return false;
  }
  return true;
}

std::pair<uint32_t, std::optional<uint32_t>>
qp::recv_awaitable::await_resume() const {
  if (exception_) [[unlikely]] {
    std::rethrow_exception(exception_);
  }
  check_wc_status(wc_.status, "failed to recv");
  if (wc_.wc_flags & IBV_WC_WITH_IMM) {
    return std::make_pair(wc_.byte_len, wc_.imm_data);
  }
  return std::make_pair(wc_.byte_len, std::nullopt);
}

qp::recv_awaitable qp::recv(local_mr *local_mr) {
  return qp::recv_awaitable(this, local_mr);
}

qp::recv_awaitable qp::recv(void *local_addr, size_t local_length,
                            uint32_t lkey) {
  return qp::recv_awaitable(this, local_addr, local_length, lkey);
}

void qp::destroy() {
  if (qp_ == nullptr) [[unlikely]] {
    return;
  }

  if (auto rc = ::ibv_destroy_qp(qp_); rc != 0) [[unlikely]] {
  } else {
  }
}

qp::~qp() { destroy(); }

} // namespace rdmapp