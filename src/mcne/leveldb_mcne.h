// ----------------------------------------------------------------------------
// MIT License
// 
// Copyright (c) 2026 HTMonkeyG
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
// ----------------------------------------------------------------------------

#ifndef STORAGE_LEVELDB_INCLUDE_MCNE_H_
#define STORAGE_LEVELDB_INCLUDE_MCNE_H_

#include "leveldb/env.h"
#include "leveldb/options.h"

namespace leveldb {

// XOR encrypted database.
class DLLX McneWrapper: public EnvWrapper {
public:
  enum {
    // 80 1D 30 01, in big endian.
    // We don't accept 90 1D 30 01 (for AES-128).
    kMagicNum = 0x01301D80,
    kMagicNumSize = 4
  };

  // Create a McneWrapper Env. If a zero-length Slice is passed, the database
  // will be considered as unencrypted.
  // We must ensure the key is valid during the database is opened. *pEnv must
  // be constructed with new operator.
  explicit McneWrapper(
    Env *pEnv,
    const Slice &key = "88329851");

  ~McneWrapper() override;

  Status NewSequentialFile(
    const std::string &fname,
    SequentialFile **result
  ) override;

  Status NewRandomAccessFile(
    const std::string &fname,
    RandomAccessFile **result
  ) override;

  Status NewWritableFile(
    const std::string &fname,
    WritableFile **result
  ) override;

  Status NewAppendableFile(
    const std::string &fname,
    WritableFile **result
  ) override;

private:
  // No copy allowed.
  McneWrapper(const McneWrapper &);
  void operator=(const McneWrapper &);

  Slice key;
};

// Automatically infer the encryption key from the database. The result is
// stored in *key. Original contents of *key are dropped. This function won't
// change the files.
//
// This function assumes that the key is 8-bytes length.
//
// If the database is unencrypted, a zero-lengthed string is stored. If the
// database is corrupted, a Status::Corrupted is returned.
//
// Env in options cannot be McneWrapper, so that the function can obtain the
// original data.
extern DLLX Status InferDB(
  const std::string &dbname,
  const Options &options,
  std::string *key);

// Encrypt (or decrypt, decided on the database) the database with the given
// key IN-PLACE. Every files of the database except log files will be circularly
// XOR encrypted with the given key.
extern DLLX Status EncryptDB(
  const std::string &dbname,
  const Options &options,
  const Slice &key);

// namespace leveldb
}

#endif
