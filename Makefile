MAKEFLAGS += -s -j

DIST_DIR = ./dist
SRC_DIR = ./deps/leveldb-mcpe

CXX = em++
CC = emcc

SRC_DIRS = $(SRC_DIR) $(wildcard $(SRC_DIR)/*/)

CFLAGS = -std=c++11 -O3 -pthread -I./deps/leveldb-mcpe -I./deps/leveldb-mcpe/include 
CFLAGS += -Wall -Wformat -Wno-unused-variable -Wno-attributes -Wno-sign-compare 
CFLAGS += -DDLLX= -DLEVELDB_PLATFORM_POSIX

LFLAGS = -lm -lnoderawfs.js -lnodefs.js -flto --bind
LFLAGS += -s USE_ZLIB=1\
	-s INITIAL_MEMORY=128MB\
	-s TOTAL_STACK=32MB\
	-s SINGLE_FILE=1\
	-s WASM=1\
	-s MODULARIZE=1\
	-s EXPORT_NAME='LevelDB'\
	-s INVOKE_RUN=0\
	-s EXPORTED_RUNTIME_METHODS=FS\
	-s FORCE_FILESYSTEM=1\
	-s NODERAWFS=1\
	-s ALLOW_MEMORY_GROWTH=1

CPP_SRC = emldb.cc\
	db/builder.cc\
	db/db_impl.cc\
	db/db_iter.cc\
	db/dbformat.cc\
	db/filename.cc\
	db/log_reader.cc\
	db/log_writer.cc\
	db/memtable.cc\
	db/repair.cc\
	db/table_cache.cc\
	db/version_edit.cc\
	db/version_set.cc\
	db/write_batch.cc\
	table/block.cc\
	table/block_builder.cc\
	table/filter_block.cc\
	table/format.cc\
	table/iterator.cc\
	table/merger.cc\
	table/table.cc\
	table/table_builder.cc\
	table/two_level_iterator.cc\
	util/arena.cc\
	util/bloom.cc\
	util/cache.cc\
	util/coding.cc\
	util/comparator.cc\
	util/crc32c.cc\
	util/env.cc\
	util/filter_policy.cc\
	util/hash.cc\
	util/histogram.cc\
	util/logging.cc\
	util/options.cc\
	util/status.cc\
	db/zlib_compressor.cc\
	db/zstd_compressor.cc\
	port/port_posix_sse.cc\
	port/port_posix.cc\
	util/env_posix.cc
CPP_OBJ = $(addprefix $(DIST_DIR)/, $(notdir $(CPP_SRC:.cc=.o)))

TARGET = libleveldb.js
BIN_TARGET = $(DIST_DIR)/$(TARGET)

vpath %.cc . $(SRC_DIRS)

$(BIN_TARGET): $(CPP_OBJ)
	@echo Linking ...
	@$(CXX) --std=c++11 $(CFLAGS) $^ -o $@ $(LFLAGS)
	@echo Done.

$(DIST_DIR)/%.o: %.cc
	@echo Compiling file "$<" ...
	@$(CXX) $(CFLAGS) -c $< -o $@

clean:
	-@del dist\*.js
	-@del dist\*.wasm
	-@del dist\*.a
	-@del dist\*.o