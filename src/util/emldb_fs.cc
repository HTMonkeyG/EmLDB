#include <emscripten.h>
#include "util/emldb_utils.h"
#include "util/emldb_fs.h"

EM_JS(i32, emfs_open, (
  cstring path,
  cstring flags,
  i32 *fdOut,
  char *message,
  size_t messageCap
), {
  try {
    HEAP32[fdOut >> 2] = require('fs').openSync(UTF8ToString(path), UTF8ToString(flags));
    return 0;
  } catch (error) {
    stringToUTF8(String((error && error.message) || error), message, messageCap);
    return (error && error.code === 'ENOENT') ? 1 : 2;
  }
});

EM_JS(i32, emfs_close, (
  i32 fd,
  char *message,
  size_t messageCap
), {
  try {
    require('fs').closeSync(fd);
    return 0;
  } catch (error) {
    stringToUTF8(String((error && error.message) || error), message, messageCap);
    return 2;
  }
});

EM_JS(i32, emfs_read, (
  i32 fd,
  u08 *dst,
  size_t length,
  f64 position,
  size_t *readOut,
  char *message,
  size_t messageCap
), {
  try {
    var target = HEAPU8.subarray(dst, dst + length);
    HEAPU32[readOut >> 2] = require('fs').readSync(
      fd, target, 0, length, position < 0 ? null : position);
    return 0;
  } catch (error) {
    stringToUTF8(String((error && error.message) || error), message, messageCap);
    return 2;
  }
});

EM_JS(i32, emfs_write, (
  i32 fd,
  const u08 *src,
  size_t length,
  size_t *writtenOut,
  char *message,
  size_t messageCap
), {
  try {
    HEAPU32[writtenOut >> 2] = require('fs').writeSync(fd, HEAPU8.subarray(src, src + length));
    return 0;
  } catch (error) {
    stringToUTF8(String((error && error.message) || error), message, messageCap);
    return 2;
  }
});

EM_JS(i32, emfs_fsync, (
  i32 fd,
  char *message,
  size_t messageCap
), {
  try {
    require('fs').fsyncSync(fd);
    return 0;
  } catch (error) {
    stringToUTF8(String((error && error.message) || error), message, messageCap);
    return 2;
  }
});

EM_JS(i32, emfs_exists, (
  cstring path
), {
  try {
    return require('fs').existsSync(UTF8ToString(path)) ? 1 : 0;
  } catch (error) {
    return 0;
  }
});

EM_JS(i32, emfs_isdir, (
  cstring path,
  i32 *isDirectoryOut,
  char *message,
  size_t messageCap
), {
  try {
    HEAP32[isDirectoryOut >> 2] = require('fs').statSync(UTF8ToString(path)).isDirectory() ? 1 : 0;
    return 0;
  } catch (error) {
    stringToUTF8(String((error && error.message) || error), message, messageCap);
    return (error && error.code === 'ENOENT') ? 1 : 2;
  }
});

EM_JS(i32, emfs_filesize, (
  cstring path,
  f64 *sizeOut,
  char *message,
  size_t messageCap
), {
  try {
    HEAPF64[sizeOut >> 3] = require('fs').statSync(UTF8ToString(path)).size;
    return 0;
  } catch (error) {
    stringToUTF8(String((error && error.message) || error), message, messageCap);
    return (error && error.code === 'ENOENT') ? 1 : 2;
  }
});

EM_JS(i32, emfs_unlink, (
  cstring path,
  char *message,
  size_t messageCap
), {
  try {
    require('fs').unlinkSync(UTF8ToString(path));
    return 0;
  } catch (error) {
    stringToUTF8(String((error && error.message) || error), message, messageCap);
    return (error && error.code === 'ENOENT') ? 1 : 2;
  }
});

EM_JS(i32, emfs_mkdir, (
  cstring path,
  char *message,
  size_t messageCap
), {
  try {
    require('fs').mkdirSync(UTF8ToString(path));
    return 0;
  } catch (error) {
    stringToUTF8(String((error && error.message) || error), message, messageCap);
    return (error && error.code === 'ENOENT') ? 1 : 2;
  }
});

EM_JS(i32, emfs_rmdir, (
  cstring path,
  char *message,
  size_t messageCap
), {
  try {
    require('fs').rmdirSync(UTF8ToString(path));
    return 0;
  } catch (error) {
    stringToUTF8(String((error && error.message) || error), message, messageCap);
    return (error && error.code === 'ENOENT') ? 1 : 2;
  }
});

EM_JS(i32, emfs_rename, (
  cstring from,
  cstring to,
  char *message,
  size_t messageCap
), {
  try {
    require('fs').renameSync(UTF8ToString(from), UTF8ToString(to));
    return 0;
  } catch (error) {
    stringToUTF8(String((error && error.message) || error), message, messageCap);
    return (error && error.code === 'ENOENT') ? 1 : 2;
  }
});

EM_JS(i32, emfs_readdir, (
  cstring path,
  u08 *buffer,
  size_t capacity,
  size_t *usedOut,
  size_t *neededOut,
  char *message,
  size_t messageCap
), {
  try {
    var names = require('fs').readdirSync(UTF8ToString(path));
    var needed = 0;
    var i;
    for (i = 0; i < names.length; i++) needed += lengthBytesUTF8(names[i]) + 1;
    HEAPU32[neededOut >> 2] = needed;
    if (needed > capacity) {
      HEAPU32[usedOut >> 2] = 0;
      return 1;
    }
    var at = 0;
    for (i = 0; i < names.length; i++) {
      var length = lengthBytesUTF8(names[i]) + 1;
      stringToUTF8(names[i], buffer + at, length);
      at += length;
    }
    HEAPU32[usedOut >> 2] = needed;
    return 0;
  } catch (error) {
    stringToUTF8(String((error && error.message) || error), message, messageCap);
    return 2;
  }
});

// We wrap all emfs function calls in a layer to prevent Emscripten from failing
// to find symbols after enabling LTO.
namespace emldb {
namespace fs {

FsRetCode open(cstring path, cstring flags, i32 *fdOut, char *message, size_t messageCap) { return (FsRetCode)emfs_open(path, flags, fdOut, message, messageCap); }
FsRetCode close(i32 fd, char *message, size_t messageCap) { return (FsRetCode)emfs_close(fd, message, messageCap); }
FsRetCode read(i32 fd, u08 *dst, size_t length, f64 position, size_t *readOut, char *message, size_t messageCap) { return (FsRetCode)emfs_read(fd, dst, length, position, readOut, message, messageCap); }
FsRetCode write(i32 fd, const u08 *src, size_t length, size_t *writtenOut, char *message, size_t messageCap) { return (FsRetCode)emfs_write(fd, src, length, writtenOut, message, messageCap); }
FsRetCode fsync(i32 fd, char *message, size_t messageCap) { return (FsRetCode)emfs_fsync(fd, message, messageCap); }
bool exists(cstring path) { return emfs_exists(path) != 0; }
FsRetCode isdir(cstring path, i32 *isDirectoryOut, char *message, size_t messageCap) { return (FsRetCode)emfs_isdir(path, isDirectoryOut, message, messageCap); }
FsRetCode filesize(cstring path, f64 *sizeOut, char *message, size_t messageCap) { return (FsRetCode)emfs_filesize(path, sizeOut, message, messageCap); }
FsRetCode unlink(cstring path, char *message, size_t messageCap) { return (FsRetCode)emfs_unlink(path, message, messageCap); }
FsRetCode mkdir(cstring path, char *message, size_t messageCap) { return (FsRetCode)emfs_mkdir(path, message, messageCap); }
FsRetCode rmdir(cstring path, char *message, size_t messageCap) { return (FsRetCode)emfs_rmdir(path, message, messageCap); }
FsRetCode rename(cstring from, cstring to, char *message, size_t messageCap) { return (FsRetCode)emfs_rename(from, to, message, messageCap); }
FsRetCode readdir(cstring path, u08 *buffer, size_t capacity, size_t *usedOut, size_t *neededOut, char *message, size_t messageCap) { return (FsRetCode)emfs_readdir(path, buffer, capacity, usedOut, neededOut, message, messageCap); }

} // namespace fs
} // namespace emldb
