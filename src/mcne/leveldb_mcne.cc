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

#include "leveldb_mcne.h"

namespace leveldb {

namespace {

// kTableMagicNumber was picked by running
//    echo http://code.google.com/p/leveldb/ | sha1sum
// and taking the leading 64 bits.
static const uint64_t kTableMagicNumber = 0xdb4775248b80fb57ull;

union EncHeader {
  char buf[McneWrapper::kMagicNumSize];
  int num;
};

// XOR encryption.
static void PerformXorOn(
  char *scratch,
  const Slice *data,
  const Slice &key,
  size_t offset
) {
  if (!key.size())
    return;

  offset %= key.size();

  for (size_t i = 0, j = offset; i < data->size(); i++, j++) {
    if (j >= key.size())
      j %= key.size();
    scratch[i] ^= key[j];
  }
}

// Check whether a file needs to be encrypted or not.
static bool MayEncrypt(
  const std::string &fname
) {
  if (fname.length() < 4)
    return true;

  // We do not encrypt these files.
  if (fname.find(".log") != std::string::npos)
    return false;
  if (fname.find("LOCK") != std::string::npos)
    return false;
  if (fname.find(".json") != std::string::npos)
    return false;
  if (fname.find(".bak") != std::string::npos)
    return false;

  return true;
}

// - McneSequentialFile.

// XOR encrypted file.
class McneSequentialFile: public SequentialFile {
public:
  McneSequentialFile(
    SequentialFile *target,
    const Slice &key
  )
    : target(target), key(key), offset(0)
  {
    assert(target != nullptr);
  }

  virtual ~McneSequentialFile() override { delete target; }

  virtual Status Read(
    size_t n,
    Slice *result,
    char *scratch
  ) override {
    // The header (magic number) have skipped in the creation.
    Status s = target->Read(n, result, scratch);
    if (s.ok()) {
      PerformXorOn(scratch, result, key, offset);
      offset += n;
    }
    return s;
  }

  virtual Status Skip(uint64_t n) override {
    Status s = target->Skip(n);
    if (s.ok())
      offset += n;
    return s;
  }

private:
  // No copy allowed.
  McneSequentialFile(const McneSequentialFile &);
  void operator=(const McneSequentialFile &);

  SequentialFile *target;
  Slice key;
  size_t offset;
};

// XOR encrypted file.
class McneRandomAccessFile: public RandomAccessFile {
public:
  explicit McneRandomAccessFile(
    RandomAccessFile *target,
    const Slice &key
  )
    : target(target), key(key)
  {
    assert(target != nullptr);
  }

  virtual ~McneRandomAccessFile() override { delete target; }

  virtual Status Read(
    uint64_t offset,
    size_t n,
    Slice *result,
    char *scratch
  ) const override {
    Status s = target->Read(McneWrapper::kMagicNumSize + offset, n, result, scratch);
    if (s.ok())
      PerformXorOn(scratch, result, key, offset);
    return s;
  }

private:
  // No copy allowed.
  McneRandomAccessFile(const McneRandomAccessFile &);
  void operator=(const McneRandomAccessFile &);

  RandomAccessFile *target;
  Slice key;
};

// XOR encrypted file.
class McneWritableFile: public WritableFile {
public:
  explicit McneWritableFile(
    WritableFile *target,
    const Slice &key
  ) 
    : target(target)
    , key(key)
    , offset(0)
  {
    assert(target != nullptr);
  }

  virtual ~McneWritableFile() override { delete target; }

  virtual Status Append(
    const Slice &data
  ) override {
    std::string localData = data.ToString();
    Slice enc = {localData};
    PerformXorOn(&localData[0], &enc, key, offset);
    Status s = target->Append(enc);
    offset += data.size();
    return s;
  }

  virtual Status Close() override { return target->Close(); }
  virtual Status Flush() override { return target->Flush(); }
  virtual Status Sync() override { return target->Sync(); }

private:
  // No copy allowed.
  McneWritableFile(const McneWritableFile &);
  void operator=(const McneWritableFile &);

  WritableFile *target;
  Slice key;
  size_t offset;
};

// namespace <annoymous>
}

// - McneWrapper.

McneWrapper::McneWrapper(
  Env *target,
  const Slice &key
)
  : EnvWrapper(target)
  , key(key)
{
  assert(target != nullptr);
}

McneWrapper::~McneWrapper() {

}

Status McneWrapper::NewSequentialFile(
  const std::string &fname,
  SequentialFile **result
) {
  // Create a SequentialFile.
  SequentialFile *pFile;
  Status s = target()->NewSequentialFile(
    fname,
    &pFile);

  if (!s.ok())
    return s;

  // Unencrypted file or database.
  if (key.size() == 0 || !MayEncrypt(fname)) {
    *result = pFile;
    return Status::OK();
  }

  // Check the magic number.
  Slice magicNumber;
  EncHeader buf;

  s = pFile->Read(
    McneWrapper::kMagicNumSize,
    &magicNumber,
    buf.buf);

  // The magic number of encrypted files does not match.
  if (!s.ok()) {
    delete pFile;
    return s;
  }
  if (buf.num != McneWrapper::kMagicNum) {
    delete pFile;
    return Status::Corruption("corrupted encrypted file");
  }

  *result = new McneSequentialFile(pFile, key);

  return Status::OK();
}

Status McneWrapper::NewRandomAccessFile(
  const std::string &fname,
  RandomAccessFile **result
) {
  // Create a SequentialFile.
  RandomAccessFile *pFile;
  Status s = target()->NewRandomAccessFile(
    fname,
    &pFile);

  if (!s.ok())
    return s;

  // Unencrypted file or database.
  if (key.size() == 0 || !MayEncrypt(fname)) {
    *result = pFile;
    return Status::OK();
  }

  // Check the magic number.
  Slice magicNumber;
  EncHeader buf;

  s = pFile->Read(
    0,
    McneWrapper::kMagicNumSize,
    &magicNumber,
    buf.buf);

  // The magic number of encrypted files does not match.
  if (!s.ok()) {
    delete pFile;
    return s;
  }
  if (buf.num != McneWrapper::kMagicNum) {
    delete pFile;
    return Status::Corruption("corrupted encrypted file");
  }

  *result = new McneRandomAccessFile(pFile, key);

  return Status::OK();
}

Status McneWrapper::NewWritableFile(
  const std::string &fname,
  WritableFile **result
) {
  // Create a WritableFile.
  WritableFile *pFile;
  Status s = target()->NewWritableFile(
    fname,
    &pFile);

  if (!s.ok())
    return s;

  // Unencrypted file or database.
  if (key.size() == 0 || !MayEncrypt(fname)) {
    *result = pFile;
    return Status::OK();
  }

  // Check the magic number.
  EncHeader buf;
  Slice magicNumber = {buf.buf, McneWrapper::kMagicNumSize};

  buf.num = McneWrapper::kMagicNum;

  s = pFile->Append(magicNumber);

  if (!s.ok()) {
    delete pFile;
    return s;
  }

  *result = new McneWritableFile(pFile, key);

  return Status::OK();
}

// We don't support AppendableFile since we can't get the encryption offset.
Status McneWrapper::NewAppendableFile(
  const std::string &fname,
  WritableFile **result
) {
  return Status::NotSupported("NewAppendableFile", fname);
}

DLLX Status InferDB(
  const std::string &dbname,
  const Options &options,
  std::string *key
) {
  Status s;
  Env *env = options.env;
  std::vector<std::string> files;

  s = env->GetChildren(dbname, &files);
  if (!s.ok())
    return s;

  if (files.empty())
    return Status::InvalidArgument(dbname, "does not exist");

  // Find MANIFEST file name - this is the known plaintext.
  std::string manifestName;
  for (auto &f : files) {
    if (f.find("MANIFEST-") == 0) {
      manifestName = f;
      break;
    }
  }

  if (manifestName.empty())
    return Status::Corruption("MANIFEST file not found", dbname);

  // Read CURRENT file raw bytes (options.env must not be McneWrapper).
  std::string currentPath = dbname + "/CURRENT";
  uint64_t currentSize;
  s = env->GetFileSize(currentPath, &currentSize);
  if (!s.ok())
    return s;

  if (currentSize < McneWrapper::kMagicNumSize) {
    // Too small to be encrypted - unencrypted database.
    key->clear();
    return Status::OK();
  }

  SequentialFile *pCurrent;
  s = env->NewSequentialFile(currentPath, &pCurrent);
  if (!s.ok())
    return s;

  std::string currentBuf(currentSize, '\0');
  Slice currentSlice;
  s = pCurrent->Read(currentSize, &currentSlice, &currentBuf[0]);
  delete pCurrent;
  if (!s.ok())
    return s;

  // Check magic number.
  EncHeader magic;
  memcpy(magic.buf, currentSlice.data(), McneWrapper::kMagicNumSize);

  if (magic.num != McneWrapper::kMagicNum) {
    // Magic number absent - unencrypted database.
    key->clear();
    return Status::OK();
  }

  // buf1: known plaintext - MANIFEST filename + newline.
  // buf2: ciphertext - CURRENT content after the 4-byte magic number.
  // key_stream[i] = buf1[i] ^ buf2[i], repeating with period 8.
  std::string buf1 = manifestName + "\n";
  const char *buf2 = currentSlice.data() + McneWrapper::kMagicNumSize;
  size_t buf2Len = currentSlice.size() - McneWrapper::kMagicNumSize;

  if (buf1.size() != buf2Len)
    return Status::Corruption("key inference failed: length mismatch");

  if (buf1.size() < 8)
    return Status::Corruption("key inference failed: plaintext too short");

  // XOR to recover key stream.
  std::string xorResult(buf1.size(), '\0');
  for (size_t i = 0; i < buf1.size(); i++)
    xorResult[i] = buf1[i] ^ buf2[i];

  // Verify the key stream repeats with period 8.
  for (size_t i = 8; i < xorResult.size(); i++) {
    if (xorResult[i] != xorResult[i & 0x07])
      return Status::Corruption("key inference failed: key stream not periodic");
  }

  *key = xorResult.substr(0, 8);
  return Status::OK();
}

DLLX Status EncryptDB(
  const std::string &dbname,
  const Options &options,
  const Slice &key
) {
  Status s;
  Env *env = options.env;
  std::vector<std::string> files;

  if (key.size() == 0)
    return Status::InvalidArgument("key", "must be non-empty");

  s = env->GetChildren(dbname, &files);
  if (!s.ok())
    return s;

  if (files.empty())
    return Status::InvalidArgument(dbname, "does not exist");

  for (auto &f: files) {
    // Skip current/parent directory entries returned by GetChildren on Windows.
    if (f == "." || f == "..")
      continue;

    std::string fpath = dbname + "/" + f;

    if (!MayEncrypt(fpath))
      continue;

    // Skip entries that are not regular files (e.g. sub-directories).
    uint64_t fsize;
    s = env->GetFileSize(fpath, &fsize);
    if (!s.ok()) {
      s = Status::OK();
      continue;
    }

    if (fsize == 0)
      continue;

    // Read the raw file content without going through McneWrapper.
    SequentialFile *pIn;
    s = env->NewSequentialFile(fpath, &pIn);
    if (!s.ok())
      return s;

    std::string rawBuf(fsize, '\0');
    Slice rawSlice;
    s = pIn->Read(fsize, &rawSlice, &rawBuf[0]);
    delete pIn;
    if (!s.ok())
      return s;

    // Decide direction by checking for the magic number.
    bool isEncrypted = false;
    if (rawSlice.size() >= McneWrapper::kMagicNumSize) {
      EncHeader magic;
      memcpy(magic.buf, rawSlice.data(), McneWrapper::kMagicNumSize);
      isEncrypted = (magic.num == McneWrapper::kMagicNum);
    }

    std::string newContent;
    if (isEncrypted) {
      // Decrypt: strip the magic header then XOR to recover plaintext.
      newContent.assign(
        rawSlice.data() + McneWrapper::kMagicNumSize,
        rawSlice.size() - McneWrapper::kMagicNumSize);
      Slice tmp(newContent);
      PerformXorOn(&newContent[0], &tmp, key, 0);
    } else {
      // Encrypt: XOR the plaintext then prepend the magic header.
      EncHeader magic;
      magic.num = McneWrapper::kMagicNum;
      std::string content = rawSlice.ToString();
      Slice tmp(content);
      PerformXorOn(&content[0], &tmp, key, 0);
      newContent.assign(magic.buf, McneWrapper::kMagicNumSize);
      newContent += content;
    }

    // Write back in-place; NewWritableFile truncates the existing file.
    WritableFile *pOut;
    s = env->NewWritableFile(fpath, &pOut);
    if (!s.ok())
      return s;

    s = pOut->Append(Slice(newContent));
    if (s.ok())
      s = pOut->Close();
    else
      pOut->Close();
    delete pOut;

    if (!s.ok())
      return s;
  }

  return Status::OK();
}

// namespace leveldb
}
