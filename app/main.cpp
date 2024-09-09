#include <cassert>
#include <iostream>
#include "include/leveldb/db.h"
int main() {
  leveldb::DB *db;
  leveldb::Options options;
  options.create_if_missing = true;
  leveldb::Status status = leveldb::DB::Open(options, "/home/zhangnf/workspace/codes/cpp/leveldb/runtime/file", &db);
  assert(status.ok());
  std::cout << "leveldb open success!" << std::endl;
  std::string value;
  std::string key1 = "testkey1";
  leveldb::Status s = db->Get(leveldb::ReadOptions(), key1, &value);
  if (s.IsNotFound()) {
    std::cout << "can not found for key:" << key1 << std::endl;
    db->Put(leveldb::WriteOptions(), key1, "testvalue1");
  }
  s = db->Get(leveldb::ReadOptions(), key1, &value);
  if (s.ok()) {
    std::cout << "found key:" << key1 << ",value:" << value << std::endl;
  }
  s = db->Delete(leveldb::WriteOptions(), key1);
  if (s.ok()) {
    std::cout << "delete key success which key:" << key1 << std::endl;
  }
  s = db->Get(leveldb::ReadOptions(), key1, &value);
  if (s.IsNotFound()) {
    std::cout << "can not found after delete for key:" << key1 << std::endl;
  }
  delete db;
  return 0;
}