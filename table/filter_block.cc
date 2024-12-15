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
  // 此处的block_offset是用来限定一个Filter最小构建粒度为kFilterBase，当Generate间隔
  // 不到一个kFilterBase时不会生成新的Filter，当Generate间隔超过kFilterBase时会按
  // kFilterBase的数量生成多条记录到filter_offsets，但它们指向的Filter都是同一个，所以
  // 注意filter的数量可能和filter_offsets的数量不一致。例如：
  // Filters:
  //    7-------12--------
  //    |Filter1|Filter2|
  // Offsets: 7,7,12
  // 这里就有3个offset，前两个7就表示指向同一个offset
  //
  // 考虑两个datablock，在sstable的范围分别是：Block 1 [0, 7KB-1], Block 2 [7KB, 14.1KB]
  // S1 首先TableBuilder为Block 1调用FilterBlockBuilder::StartBlock(0)，该函数直接返回；
  // S2 然后依次向Block 1加入k/v，其中会调用FilterBlockBuilder::AddKey，FilterBlockBuilder记录这些key。
  // S3 下一次TableBuilder添加k/v时，例行检查发现Block 1的大小超过设置，则执行Flush操作，Flush操作在写入Block 1后，开始准备Block 2并更新block offset=7KB，最后调用FilterBlockBuilder::StartBlock(7KB)，开始为Block 2构建Filter。
  // S4 在FilterBlockBuilder::StartBlock(7KB)中，计算出filter index = 3，触发3次GenerateFilter函数，为Block 1添加的那些key列表创建filter，其中第2、3次循环创建的是空filter。
  //    在StartBlock(7KB)时会向filter的偏移数组filter_offsets_压入两个包含空key set的元素，filter_offsets_[1]和filter_offsets_[2]，它们的值都等于7KB-1。
  // S5 Block 2构建结束，TableBuilder调用Finish结束table的构建，这会再次触发Flush操作，在写入Block 2后，为Block 2的key创建filter。
  //    这里如果Block 1的范围是[0, 1.8KB-1]，Block 2从1.8KB开始，那么Block 2将会和Block 1共用一个filter，它们的filter都被生成到filter 0中。 当然在TableBuilder构建表时，Block的大小是根据参数配置的，也是基本均匀的。
  //
  uint64_t filter_index = (block_offset / kFilterBase);
  assert(filter_index >= filter_offsets_.size());
  while (filter_index > filter_offsets_.size()) {
    GenerateFilter();
  }
}

void FilterBlockBuilder::AddKey(const Slice& key) {
  Slice k = key;
  // start中每一个元素代表的都是keys(std::string)中的offset
  start_.push_back(keys_.size());
  keys_.append(k.data(), k.size());
}

Slice FilterBlockBuilder::Finish() {
  if (!start_.empty()) {
    GenerateFilter();
  }

  // step1.挨个放置filter在result_中的offset
  const uint32_t array_offset = result_.size();
  for (size_t i = 0; i < filter_offsets_.size(); i++) {
    PutFixed32(&result_, filter_offsets_[i]);
  }

  // step2.写入上一步的offset数量
  PutFixed32(&result_, array_offset);
  // step3.写入filter分段大小（2KB）
  result_.push_back(kFilterBaseLg);
  return Slice(result_);
}

void FilterBlockBuilder::GenerateFilter() {
  // 方法末尾会清空临时变量
  const size_t num_keys = start_.size();
  if (num_keys == 0) {
    // Fast path if there are no keys for this filter
    filter_offsets_.push_back(result_.size());
    return;
  }

  // Make list of keys from flattened key structure
  start_.push_back(keys_.size());  // Simplify length computation
  tmp_keys_.resize(num_keys);
  for (size_t i = 0; i < num_keys; i++) {
    const char* base = keys_.data() + start_[i];
    size_t length = start_[i + 1] - start_[i];
    tmp_keys_[i] = Slice(base, length);
  }

  // Generate filter for current set of keys and append to result_.
  filter_offsets_.push_back(result_.size());
  // &tmp_keys_[0]是取首元素的指针
  policy_->CreateFilter(&tmp_keys_[0], static_cast<int>(num_keys), &result_);

  tmp_keys_.clear();
  keys_.clear();
  start_.clear();
}

FilterBlockReader::FilterBlockReader(const FilterPolicy* policy,
                                     const Slice& contents)
    : policy_(policy), data_(nullptr), offset_(nullptr), num_(0), base_lg_(0) {
  size_t n = contents.size();
  if (n < 5) return;  // 1 byte for base_lg_ and 4 for start of offset array
  base_lg_ = contents[n - 1];
  // last_word是filter_offsets数组的起始位置(相对)
  uint32_t last_word = DecodeFixed32(contents.data() + n - 5);
  if (last_word > n - 5) return;
  // data是起始位置（绝对）
  data_ = contents.data();
  // offset是filter_offsets数组的起始位置(绝对)
  offset_ = data_ + last_word;
  // n-5是filter_offsets数组的末尾位置，last_word是起始位置，相减后/4即filter_offsets的
  // 数量
  num_ = (n - 5 - last_word) / 4;
}

bool FilterBlockReader::KeyMayMatch(uint64_t block_offset, const Slice& key) {
  uint64_t index = block_offset >> base_lg_;
  if (index < num_) {
    // block_offset所对应的filter的起始offset（相对位置）
    uint32_t start = DecodeFixed32(offset_ + index * 4);
    // block_offset所对应的下一个filter的起始offset，即所对应filter的结束offset（相对位置）
    uint32_t limit = DecodeFixed32(offset_ + index * 4 + 4);
    if (start <= limit && limit <= static_cast<size_t>(offset_ - data_)) {
      Slice filter = Slice(data_ + start, limit - start);
      return policy_->KeyMayMatch(key, filter);
    } else if (start == limit) {
      // Empty filters do not match any keys
      return false;
    }
  }
  return true;  // Errors are treated as potential matches
}

}  // namespace leveldb
