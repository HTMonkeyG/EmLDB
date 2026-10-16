#ifndef STORAGE_LEVELDB_UTIL_EMLDB_FS_H_
#define STORAGE_LEVELDB_UTIL_EMLDB_FS_H_

#include "util/emldb_utils.h"

// ----------------------------------------------------------------------------
// FileSystem Functions
// ----------------------------------------------------------------------------

namespace emldb {

enum FsRetCode: i32 {
  kFsOk = 0,
  kFsNotFound = 1,
  kFsError = 2,
};

namespace fs {

FsRetCode open(
  cstring path,
  cstring flags,
  i32 *fdOut,
  char *message,
  size_t messageCap);

FsRetCode close(
  i32 fd,
  char *message,
  size_t messageCap);

// A negative `position` reads from the descriptor's own offset.
FsRetCode read(
  i32 fd,
  u08 *dst,
  size_t length,
  f64 position,
  size_t *readOut,
  char *message,
  size_t messageCap);

FsRetCode write(
  i32 fd,
  const u08 *src,
  size_t length,
  size_t *writtenOut,
  char *message,
  size_t messageCap);

FsRetCode fsync(
  i32 fd,
  char *message,
  size_t messageCap);

bool exists(
  cstring path);

FsRetCode isdir(
  cstring path,
  i32 *isDirectoryOut,
  char *message,
  size_t messageCap);

FsRetCode filesize(
  cstring path,
  f64 *sizeOut,
  char *message,
  size_t messageCap);

FsRetCode unlink(
  cstring path,
  char *message,
  size_t messageCap);

FsRetCode mkdir(
  cstring path,
  char *message,
  size_t messageCap);

FsRetCode rmdir(
  cstring path,
  char *message,
  size_t messageCap);

FsRetCode rename(
  cstring from,
  cstring to,
  char *message,
  size_t messageCap);

// Writes the directory listing as NUL-separated names and reports how many
// bytes it needs, so the caller can retry with a bigger buffer.
// Returns kEmFsOk, 1 when `capacity` is too small, or kEmFsError.
FsRetCode readdir(
  cstring path,
  u08 *buffer,
  size_t capacity,
  size_t *usedOut,
  size_t *neededOut,
  char *message,
  size_t messageCap);

}

}

#endif
