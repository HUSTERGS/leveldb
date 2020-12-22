// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "table/filter_block.h"

#include "leveldb/filter_policy.h"
#include "util/coding.h"

namespace leveldb {

// See doc/table_format.md for an explanation of the filter block format.

// Generate new filter every 2KB of data
static const size_t kFilterBaseLg = 11;
static const size_t kFilterBase = 1 << kFilterBaseLg;

FilterBlockBuilder::FilterBlockBuilder(const FilterPolicy* policy)
    : policy_(policy) {}

void FilterBlockBuilder::StartBlock(uint64_t block_offset) {
  // 新建一个新的filter，那么对应的block的位置应该没有记录在filter_offset_中
  uint64_t filter_index = (block_offset / kFilterBase);
  // 其实可以理解这个地方的循环就是为了不断的填充filter_offsets_，在GenerateFilter中的
  // 判断中会由于num_keys为0而直接跳出循环，仅仅是在filter_offsets_中新增了一个成员
  // 但是为什么会出现这种情况呢，是因为调用StartBlock的时候的block_offset参数可能不是连续单增的吗
  // block出现空洞？
  assert(filter_index >= filter_offsets_.size());
  while (filter_index > filter_offsets_.size()) {
    GenerateFilter();
  }
}

void FilterBlockBuilder::AddKey(const Slice& key) {
  Slice k = key;
  // 由于keys_就是一个string，所以可以理解为，start_记录了当前key在keys_中的起始偏移量
  start_.push_back(keys_.size());
  keys_.append(k.data(), k.size());
}

Slice FilterBlockBuilder::Finish() {
  // 其实检测start_和keys_效果是一样的，但是遇到这种判定的我总是觉得是不是和具体函数中的顺序有关
  // 比如说AddKey中是先push_back start_然后才对keys_进行append，包括后面的GenerateFilter函数
  // 就是反过来，先对keys_进行clear，再对start_进行clear，感觉某种程度上增加了代码的健壮性？
  if (!start_.empty()) {
    GenerateFilter();
  }

  // Append array of per-filter offsets
  const uint32_t array_offset = result_.size();
  for (size_t i = 0; i < filter_offsets_.size(); i++) {
    PutFixed32(&result_, filter_offsets_[i]);
  }
  // 标明array的开始位置
  PutFixed32(&result_, array_offset);
  result_.push_back(kFilterBaseLg);  // Save encoding parameter in result
  return Slice(result_);
}

void FilterBlockBuilder::GenerateFilter() {
  // 目前需要计算的key的个数
  const size_t num_keys = start_.size();
  if (num_keys == 0) {
    // Fast path if there are no keys for this filter
    // 没看懂，我猜是每一个filter_offset_中的成员应该是递增的，如果没有递增，像这种情况，就说明没有keys
    filter_offsets_.push_back(result_.size());
    return;
  }

  // Make list of keys from flattened key structure
  start_.push_back(keys_.size());  // Simplify length computation
  tmp_keys_.resize(num_keys);
  // 这些创建的Slice，并没有占用新的空间，而是直接指向了keys_，但是还是有点好奇一段长的string和多个短的string相比
  // 会更好吗。可能我就会直接创建一个Slice的vector，或者string的vector
  // 但是AddKey部分，传入的参数是Slice，所以为了避免上层维护的data出现问题还是会不可避免的进行一次数据的复制？
  // 也就是AddKyes中的keys_.append(k.data(), k.size())
  for (size_t i = 0; i < num_keys; i++) {
    const char* base = keys_.data() + start_[i];
    size_t length = start_[i + 1] - start_[i];
    tmp_keys_[i] = Slice(base, length);
  }

  // Generate filter for current set of keys and append to result_.
  filter_offsets_.push_back(result_.size());
  // 所以CreateFilter方法需要直接在result_参数中向后加，而不是赋一个新的值
  policy_->CreateFilter(&tmp_keys_[0], static_cast<int>(num_keys), &result_);

  tmp_keys_.clear();
  keys_.clear();
  start_.clear();
}

FilterBlockReader::FilterBlockReader(const FilterPolicy* policy,
                                     const Slice& contents)
    : policy_(policy), data_(nullptr), offset_(nullptr), num_(0), base_lg_(0) {
  size_t n = contents.size();
  // 可以对照着上面的Finish函数，当极端条件下filter_offsets_为空的时候，至少需要4 bytes用来
  // 存放PutFixed32，保存的array的开始位置
  // 以及一个byte保存的kFilterBaseLg
  if (n < 5) return;  // 1 byte for base_lg_ and 4 for start of offset array
  base_lg_ = contents[n - 1];
  uint32_t last_word = DecodeFixed32(contents.data() + n - 5);
  // 标志的array开始位置不应该比元数据还后面
  if (last_word > n - 5) return;
  data_ = contents.data();
  offset_ = data_ + last_word; // 指针的偏移，直接指向offset数组
  // 因为offset部分，每一个offset都是通过PutFixed32来插入数据的，所以通过获得offset数组的总长度
  // 并除以4就可以得到总共存储的offset或者说entry的个数
  num_ = (n - 5 - last_word) / 4;
}

bool FilterBlockReader::KeyMayMatch(uint64_t block_offset, const Slice& key) {
  // 每2kb的数据，作为一个block的数据，对应一个filter的entry，也就是对应一个offset
  // 通过将Block的offset除以2kb，可以得到具体的filter entry
  uint64_t index = block_offset >> base_lg_;
  if (index < num_) {
    // start, limit 都是对应的data_ string中的偏移量，通过前后两个offset可以得到当前对应的filter的大小
    uint32_t start = DecodeFixed32(offset_ + index * 4);
    uint32_t limit = DecodeFixed32(offset_ + index * 4 + 4);
    if (start <= limit && limit <= static_cast<size_t>(offset_ - data_)) {
      Slice filter = Slice(data_ + start, limit - start);
      return policy_->KeyMayMatch(key, filter);
    } else if (start == limit) {
      // 这个地方其实只包含了一种情况，就是offset刚好是对应的最后一个entry的情况
      // 如果仅仅是因为filter是空的，导致start和limit相等的话，就会直接在上一个if中
      // 由KeyMayMatch来操作，只是filter的长度为0
      // 所以这个情况应该是对应着最后一个entry的时候，start != limit？？？
      // 说不通了
      // Empty filters do not match any keys
      return false;
    }
  }
  // 如果出错就认为有可能找到，那么就会进行实际的查询
  return true;  // Errors are treated as potential matches
}

}  // namespace leveldb
