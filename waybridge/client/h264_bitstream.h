#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// NVENC CBR streams may end an access unit in filler_data (NAL type 12).
// Microsoft's decoder can then wait for the next input before releasing this
// picture. Padding carries no reference/picture data and can be omitted safely.
inline size_t copyH264WithoutFiller(uint8_t *dest, const uint8_t *data, size_t length) {
  auto startCode = [&](size_t from, size_t &prefix) {
    for (size_t i = from; i + 3 <= length; ++i) {
      if (data[i] || data[i + 1])
        continue;
      if (data[i + 2] == 1) { prefix = 3; return i; }
      if (i + 4 <= length && !data[i + 2] && data[i + 3] == 1) {
        prefix = 4; return i;
      }
    }
    prefix = 0;
    return length;
  };
  size_t prefix = 0, start = startCode(0, prefix), copied = 0, keep = 0;
  while (start < length) {
    size_t nextPrefix = 0, next = startCode(start + prefix, nextPrefix);
    if (start + prefix < length && (data[start + prefix] & 31) == 12) {
      memcpy(dest + copied, data + keep, start - keep);
      copied += start - keep;
      keep = next;
    }
    start = next;
    prefix = nextPrefix;
  }
  memcpy(dest + copied, data + keep, length - keep);
  return copied + length - keep;
}
