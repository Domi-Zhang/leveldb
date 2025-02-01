// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/db_iter.h"

#include "db/db_impl.h"
#include "db/dbformat.h"
#include "db/filename.h"
#include "leveldb/env.h"
#include "leveldb/iterator.h"
#include "port/port.h"
#include "util/logging.h"
#include "util/mutexlock.h"
#include "util/random.h"

namespace leveldb {

#if 0
static void DumpInternalIter(Iterator* iter) {
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    ParsedInternalKey k;
    if (!ParseInternalKey(iter->key(), &k)) {
      std::fprintf(stderr, "Corrupt '%s'\n", EscapeString(iter->key()).c_str());
    } else {
      std::fprintf(stderr, "@ '%s'\n", k.DebugString().c_str());
    }
  }
}
#endif

namespace {

// Memtables and sstables that make the DB representation contain
// (userkey,seq,type) => uservalue entries.  DBIter
// combines multiple entries for the same userkey found in the DB
// representation into a single entry while accounting for sequence
// numbers, deletion markers, overwrites, etc.

// DBIter 有一个特性如下： 执行完 Prev 后迭代器的saved_key 即是迭代器当前的 user_key，而
// 指针指示的位置是当前 saved_key 节点的上一个节点(Prev 方向)。 这一点不同于 Next，Next
// 的指针所指位置的 user_key 即是迭代器当前的 user_key。 造成这个不同的关键原因是:
// Internalkey 排序规则是 Internal key 的排序规则: user_key 正序 -->
// sequence number 逆序 --> type 逆序，那么相同 user_key 的更新的版本会排在前面
// (Prev 方向), 所以要找到当前 user_key 的前面一个有效 user_key, 必须要把 user_key
// 的所有节点都遍历完, 才会得知这个 user_key 是否没有被删除, 或是否有更新版本的值。
class DBIter : public Iterator {
 public:
  // Which direction is the iterator currently moving?
  // (1) When moving forward, the internal iterator is positioned at
  //     the exact entry that yields this->key(), this->value()
  // (2) When moving backwards, the internal iterator is positioned
  //     just before all entries whose user key == this->key().
  enum Direction { kForward, kReverse };

  DBIter(DBImpl* db, const Comparator* cmp, Iterator* iter, SequenceNumber s,
         uint32_t seed)
      : db_(db),
        user_comparator_(cmp),
        iter_(iter), // 在当前实现中,iter_始终只有MergingIterator这一种
        sequence_(s),
        direction_(kForward),
        valid_(false),
        rnd_(seed),
        bytes_until_read_sampling_(RandomCompactionPeriod()) {}

  DBIter(const DBIter&) = delete;
  DBIter& operator=(const DBIter&) = delete;

  ~DBIter() override { delete iter_; }
  bool Valid() const override { return valid_; }
  Slice key() const override {
    assert(valid_);
    return (direction_ == kForward) ? ExtractUserKey(iter_->key()) : saved_key_;
  }
  Slice value() const override {
    assert(valid_);
    return (direction_ == kForward) ? iter_->value() : saved_value_;
  }
  Status status() const override {
    if (status_.ok()) {
      return iter_->status();
    } else {
      return status_;
    }
  }

  void Next() override;
  void Prev() override;
  void Seek(const Slice& target) override;
  void SeekToFirst() override;
  void SeekToLast() override;

 private:
  void FindNextUserEntry(bool skipping, std::string* skip);
  void FindPrevUserEntry();
  bool ParseKey(ParsedInternalKey* key);

  inline void SaveKey(const Slice& k, std::string* dst) {
    dst->assign(k.data(), k.size());
  }

  inline void ClearSavedValue() {
    if (saved_value_.capacity() > 1048576) {
      std::string empty;
      swap(empty, saved_value_);
    } else {
      saved_value_.clear();
    }
  }

  // Picks the number of bytes that can be read until a compaction is scheduled.
  size_t RandomCompactionPeriod() {
    return rnd_.Uniform(2 * config::kReadBytesPeriod);
  }

  DBImpl* db_;
  const Comparator* const user_comparator_;
  Iterator* const iter_; // 在当前实现中,iter_始终只有MergingIterator这一种
  SequenceNumber const sequence_;
  Status status_;
  std::string saved_key_;    // == current key when direction_==kReverse
  std::string saved_value_;  // == current raw value when direction_==kReverse
  Direction direction_;
  bool valid_;
  Random rnd_;
  // 在下一次compaction之前剩余的可读字节数
  size_t bytes_until_read_sampling_;
};

// 读取iter_指向的key/value，其中可能会触发compaction
inline bool DBIter::ParseKey(ParsedInternalKey* ikey) {
  Slice k = iter_->key();

  size_t bytes_read = k.size() + iter_->value().size();
  // 剩余可读字节数已经小于本次要读的数据长度了
  while (bytes_until_read_sampling_ < bytes_read) {
    // 复位可读字节数
    bytes_until_read_sampling_ += RandomCompactionPeriod();
    // 记录当前这次读取的字节数，这个方法内部(可能)会触发compaction
    db_->RecordReadSample(k);
  }
  assert(bytes_until_read_sampling_ >= bytes_read);
  bytes_until_read_sampling_ -= bytes_read;

  if (!ParseInternalKey(k, ikey)) {
    status_ = Status::Corruption("corrupted internal key in DBIter");
    return false;
  } else {
    return true;
  }
}

void DBIter::Next() {
  assert(valid_);

  if (direction_ == kReverse) {  // Switch directions?
    direction_ = kForward;
    // iter_ is pointing just before the entries for this->key(),
    // so advance into the range of entries for this->key() and then
    // use the normal skipping code below.
    if (!iter_->Valid()) {
      iter_->SeekToFirst();
    } else {
      iter_->Next();
    }
    if (!iter_->Valid()) {
      valid_ = false;
      saved_key_.clear();
      return;
    }
    // saved_key_ already contains the key to skip past.
  } else {
    // Store in saved_key_ the current key so we skip it below.
    SaveKey(ExtractUserKey(iter_->key()), &saved_key_);

    // iter_ is pointing to current key. We can now safely move to the next to
    // avoid checking current key.
    // DbIter::key()方法，在direction_ = kForward时取的是iter_->key()，所以此时
    // saved_key中存储的其实是上一条Key，iter_->Next()方法让指针往后移动一次，指向下一个key，
    // 同时会通过FindNextUserEntry跳过更小的version或被deleted的记录
    iter_->Next();
    if (!iter_->Valid()) {
      valid_ = false;
      saved_key_.clear();
      return;
    }
  }

  FindNextUserEntry(true, &saved_key_);
}

// 不断查看iter_当前的key，判断其是否需要跳过(例如deleted或更小的seq_num)，如果是的话就让
// iter_-Next()，否则退出。也就是这个方法的核心是Next()，并不返回什么值。
// skipping- 是否要跳过同user_key但sequence_num更小的记录，skip-存储被跳过的user_key
// skip- 在方法进来的时候，设置的是上一条记录的key，在方法退出的时候存的是被跳过的key(如果发
// 生了skip)，这是个双向参数
void DBIter::FindNextUserEntry(bool skipping, std::string* skip) {
  // Loop until we hit an acceptable entry to yield
  assert(iter_->Valid());
  assert(direction_ == kForward);
  do {
    ParsedInternalKey ikey;
    if (ParseKey(&ikey) && ikey.sequence <= sequence_) {
      switch (ikey.type) {
        case kTypeDeletion:
          // Arrange to skip all upcoming entries for this key since
          // they are hidden by this deletion.
          // ikey有user_key、sequence和type三个字段，按照user_key正序、sequence和type
          // 降序排列。当读到一个type=kTypeDeletion时，后面同user_key（注意是user_key不
          // 是ikey）就意味着已经被删掉了，需要skip掉
          SaveKey(ikey.user_key, skip);
          skipping = true;
          break;
        case kTypeValue:
          if (skipping &&
              user_comparator_->Compare(ikey.user_key, *skip) <= 0) {
            // Entry hidden
            // 这里的<似乎不太需要？因为user_key已经是单调递增的
          } else {
            // 如果不跳过(skipping==false)或者user_key已经切换到下一条，则停止遍历
            valid_ = true;
            saved_key_.clear();
            return;
          }
          break;
      }
    }
    iter_->Next();
  } while (iter_->Valid());
  saved_key_.clear();
  valid_ = false;
}

void DBIter::Prev() {
  assert(valid_);

  if (direction_ == kForward) {  // Switch directions?
    // iter_ is pointing at the current entry.  Scan backwards until
    // the key changes so we can use the normal reverse scanning code.
    assert(iter_->Valid());  // Otherwise valid_ would have been false
    SaveKey(ExtractUserKey(iter_->key()), &saved_key_);
    // 此时saved_key_指向的是iter_->key()，我们需要不断调用iter_->Prev()，使其指向上一个
    // 不一样的user_key，逻辑可以参见FindPrevUserEntry()函数
    while (true) {
      iter_->Prev();
      if (!iter_->Valid()) {
        valid_ = false;
        saved_key_.clear();
        ClearSavedValue();
        return;
      }
      // 这里等同于FindPrevUserEntry()函数中while的停止条件：
      // if ((value_type != kTypeDeletion) &&
      //            user_comparator_->Compare(ikey.user_key, saved_key_) < 0)
      // kForward时当前value_type肯定不是kTypeDeletion，所以可以忽略此条件，即成为下面这
      // 条判断语句。
      if (user_comparator_->Compare(ExtractUserKey(iter_->key()), saved_key_) <
          0) {
        break;
      }
    }
    direction_ = kReverse;
  }

  FindPrevUserEntry();
}

void DBIter::FindPrevUserEntry() {
  assert(direction_ == kReverse);

  ValueType value_type = kTypeDeletion;
  if (iter_->Valid()) {
    do {
      ParsedInternalKey ikey;
      if (ParseKey(&ikey) && ikey.sequence <= sequence_) {
        if ((value_type != kTypeDeletion) &&
            user_comparator_->Compare(ikey.user_key, saved_key_) < 0) {
          // We encountered a non-deleted value in entries for previous keys,
          // 这个if分支表示遍历到了<更前一位>的user key，且刚刚遍历到的kv是有效的(第一个判
          // 断条件)，而且由于seq_num的倒序排列的特点，刚刚遍历到的user_key就是最新的，
          // 这意味着我们可以跳出while，此时想要的user_key存储在saved_key_中，（而不是像
          // kForward，直接存储在iter_->key()）。
          // 记住ikey=user_key(正序)+seq_num(倒序)+type(倒序)，只有当相同的user_key
          // 遍历完了才能决定这个user_key的最新值、是否被deleted了
          break;
        }
        value_type = ikey.type;
        if (value_type == kTypeDeletion) {
          saved_key_.clear();
          ClearSavedValue();
        } else {
          Slice raw_value = iter_->value();
          if (saved_value_.capacity() > raw_value.size() + 1048576) {
            std::string empty;
            swap(empty, saved_value_);
          }
          SaveKey(ExtractUserKey(iter_->key()), &saved_key_);
          saved_value_.assign(raw_value.data(), raw_value.size());
        }
      }
      iter_->Prev();
    } while (iter_->Valid());
  }

  if (value_type == kTypeDeletion) {
    // End
    valid_ = false;
    saved_key_.clear();
    ClearSavedValue();
    direction_ = kForward;
  } else {
    valid_ = true;
  }
}

void DBIter::Seek(const Slice& target) {
  direction_ = kForward;
  ClearSavedValue();
  saved_key_.clear();
  AppendInternalKey(&saved_key_,
                    ParsedInternalKey(target, sequence_, kValueTypeForSeek));
  iter_->Seek(saved_key_);
  if (iter_->Valid()) {
    FindNextUserEntry(false, &saved_key_ /* temporary storage */);
  } else {
    valid_ = false;
  }
}

void DBIter::SeekToFirst() {
  direction_ = kForward;
  ClearSavedValue();
  iter_->SeekToFirst();
  if (iter_->Valid()) {
    FindNextUserEntry(false, &saved_key_ /* temporary storage */);
  } else {
    valid_ = false;
  }
}

void DBIter::SeekToLast() {
  direction_ = kReverse;
  ClearSavedValue();
  iter_->SeekToLast();
  FindPrevUserEntry();
}

}  // anonymous namespace

Iterator* NewDBIterator(DBImpl* db, const Comparator* user_key_comparator,
                        Iterator* internal_iter, SequenceNumber sequence,
                        uint32_t seed) {
  return new DBIter(db, user_key_comparator, internal_iter, sequence, seed);
}

}  // namespace leveldb
