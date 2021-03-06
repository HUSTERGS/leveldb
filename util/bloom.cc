// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/filter_policy.h"

#include "leveldb/slice.h"
#include "util/hash.h"

namespace leveldb {

namespace {
static uint32_t BloomHash(const Slice& key) {
  return Hash(key.data(), key.size(), 0xbc9f1d34);
}

class BloomFilterPolicy : public FilterPolicy {
 public:
  explicit BloomFilterPolicy(int bits_per_key) : bits_per_key_(bits_per_key) {
    // We intentionally round down to reduce probing cost a little bit
    // 什么意思，"降低探测成本"，因为一个key需要对每一个hash函数都进行计算，然后需要减少计算的函数的个数吗
    // 当有长度为m的位数组以及n个数据的时候，最优的hash函数的个数k = ln(2) * m / n
    // 上述结论可以参考 https://blog.csdn.net/jiaomeng/article/details/1495500
    // 而m / n就是每一个元素对应的bit的个数，也就是参数中的bits_per_key
    k_ = static_cast<size_t>(bits_per_key * 0.69);  // 0.69 =~ ln(2)
    if (k_ < 1) k_ = 1;
    if (k_ > 30) k_ = 30;
  }

  const char* Name() const override { return "leveldb.BuiltinBloomFilter2"; }

  void CreateFilter(const Slice* keys, int n, std::string* dst) const override {
    // Compute bloom filter size (in both bits and bytes)
    size_t bits = n * bits_per_key_; // n * bits_per_key_ = m，也就是位数组的长度
    // n个key
    // For small n, we can see a very high false positive rate.  Fix it
    // by enforcing a minimum bloom filter length.
    if (bits < 64) bits = 64;

    size_t bytes = (bits + 7) / 8;
    bits = bytes * 8;

    const size_t init_size = dst->size();
    dst->resize(init_size + bytes, 0);
    dst->push_back(static_cast<char>(k_));  // Remember # of probes in filter
    char* array = &(*dst)[init_size]; // 获得开始的地址，在k_之后
    // 对于每一个key
    for (int i = 0; i < n; i++) {
      // Use double-hashing to generate a sequence of hash values.
      // See analysis in [Kirsch,Mitzenmacher 2006].
      uint32_t h = BloomHash(keys[i]);

      // 交换低17位和高15位的位置
      // 原来： [15 bits][17 bits]
      // 交换之后: [17 bits][15 bits]
      const uint32_t delta = (h >> 17) | (h << 15);  // Rotate right 17 bits
      // 对于每一个hash函数
      for (size_t j = 0; j < k_; j++) {
        // 由于hash值需要映射在长度为m的位数组中，所以需要mod bits，mod的结果指示了应该将m中的哪一位，置为1
        const uint32_t bitpos = h % bits;
        // % 8之后的范围在0-7，然后1左移0-7位，也就是遍历了第0位是1直到第7位是1的情况，正确的将对应的位置设置为1
        array[bitpos / 8] |= (1 << (bitpos % 8));
        // 这个地方没懂
        // 懂了，就实际上并没有真的设置k_个hash函数，而是通过单一的hash函数(BloomHash)以及上述delta的方法，每一次增加
        // delta，来得到一系列的hash值，惊呆了
        h += delta;
      }
    }
  }

  bool KeyMayMatch(const Slice& key, const Slice& bloom_filter) const override {
    const size_t len = bloom_filter.size();
    if (len < 2) return false;

    const char* array = bloom_filter.data();
    const size_t bits = (len - 1) * 8;

    // Use the encoded k so that we can read filters generated by
    // bloom filters created using different parameters.
    const size_t k = array[len - 1];
    if (k > 30) {
      // Reserved for potentially new encodings for short bloom filters.
      // Consider it a match.
      return true;
    }

    uint32_t h = BloomHash(key);
    const uint32_t delta = (h >> 17) | (h << 15);  // Rotate right 17 bits
    for (size_t j = 0; j < k; j++) {
      const uint32_t bitpos = h % bits;
      if ((array[bitpos / 8] & (1 << (bitpos % 8))) == 0) return false;
      h += delta;
    }
    return true;
  }

 private:
  size_t bits_per_key_;
  size_t k_;
};
}  // namespace

const FilterPolicy* NewBloomFilterPolicy(int bits_per_key) {
  return new BloomFilterPolicy(bits_per_key);
}

}  // namespace leveldb
