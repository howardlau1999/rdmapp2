#include "rdmapp/executor.h"

#include <coroutine>

namespace rdmapp {

void process_wc(struct ibv_wc const &wc) {
  struct ibv_wc *wc_ptr = reinterpret_cast<struct ibv_wc *>(wc.wr_id);
  *wc_ptr = wc;
  std::coroutine_handle<> h = std::coroutine_handle<>::from_address(
      *reinterpret_cast<void **>(wc_ptr + 1));
  h.resume();
}

} // namespace rdmapp