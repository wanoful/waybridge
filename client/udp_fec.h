#pragma once
#include <cstddef>
#include <cstdint>

#if defined(WAYBRIDGE_RS_FEC)
#include "../native/udp_fec.h"
#include <memory>

inline constexpr bool ReedSolomonAvailable = true;
inline size_t recoverReedSolomon(uint8_t *body, size_t total, uint8_t *seen, size_t start,
                                 const uint8_t *parity0, const uint8_t *parity1) {
  static const bool initialized = [] { wb_rs_initialize(); return true; }();
  static thread_local std::unique_ptr<WbReedSolomon, decltype(&wb_rs_destroy)>
      codec(initialized ? wb_rs_create() : nullptr, wb_rs_destroy);
  return wb_rs_recover(codec.get(), body, total, seen, start, parity0, parity1);
}
#else
inline constexpr bool ReedSolomonAvailable = false;
inline size_t recoverReedSolomon(uint8_t *, size_t, uint8_t *, size_t,
                                 const uint8_t *, const uint8_t *) {
  return 0;
}
#endif
