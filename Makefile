MAKEFLAGS += -s -j6
.DEFAULT_GOAL := all

DIST_DIR = ./dist
LEVELDB_DIR = ./deps/leveldb-mcpe
SRC_DIR = ./src

CXX = em++
CC = emcc

SRC_DIRS = $(SRC_DIR) $(wildcard $(SRC_DIR)/*/) $(LEVELDB_DIR) $(wildcard $(LEVELDB_DIR)/*/)

CFLAGS = -std=c++11 -O3 -flto -pthread -fexceptions -DNDEBUG -MMD -MP -s USE_ZLIB=1
# Keep the tracked Emscripten port shim ahead of the submodule so a clean
# checkout does not depend on local edits inside deps/leveldb-mcpe.
CFLAGS += -I./src -I./deps/leveldb-mcpe -I./deps/leveldb-mcpe/include
CFLAGS += -DDLLX= -DLEVELDB_PLATFORM_EMSCRIPTEN
CFLAGS += -Wall -Wformat -Wno-unused-variable -Wno-attributes -Wno-sign-compare

LFLAGS_COMMON = -lm -flto --bind -msimd128 -fexceptions
LFLAGS_COMMON += -s USE_ZLIB=1\
	-s TOTAL_STACK=4MB\
	-s SINGLE_FILE=1\
	-s WASM=1\
	-s MODULARIZE=1\
	-s EXPORT_NAME='LevelDB'\
	-s INVOKE_RUN=0\
	-s ENVIRONMENT=node\
	-s PTHREAD_POOL_SIZE=2

# The default artifact uses a fixed heap for predictable pthread performance.
# The growth artifact trades some peak performance for a 2GB ceiling.
LFLAGS_FIXED = $(LFLAGS_COMMON) -s INITIAL_MEMORY=128MB -s ALLOW_MEMORY_GROWTH=0
LFLAGS_GROWTH = $(LFLAGS_COMMON) -s INITIAL_MEMORY=32MB -s MAXIMUM_MEMORY=2GB -s ALLOW_MEMORY_GROWTH=1 -Wno-pthreads-mem-growth

CPP_SRC = db/builder.cc\
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
	port/port_emscripten.cc\
	util/emldb_utils.cc\
	util/env_emscripten.cc\
	mcne/leveldb_mcne.cc\
	emldb.cc
CPP_OBJ = $(addprefix $(DIST_DIR)/, $(notdir $(CPP_SRC:.cc=.o)))
DEP_FILES = $(CPP_OBJ:.o=.d)

FIXED_TARGET = $(DIST_DIR)/libleveldb.js
GROWTH_TARGET = $(DIST_DIR)/libleveldb-growth.js

vpath %.cc . $(SRC_DIRS)

all: $(FIXED_TARGET) $(GROWTH_TARGET)

$(FIXED_TARGET): $(CPP_OBJ)
	@echo Linking performance profile ...
	@$(CXX) --std=c++11 $(CFLAGS) $^ -o $@ $(LFLAGS_FIXED)
	@echo Done.

$(GROWTH_TARGET): $(CPP_OBJ)
	@echo Linking scalable profile ...
	@$(CXX) --std=c++11 $(CFLAGS) $^ -o $@ $(LFLAGS_GROWTH)
	@echo Done.

$(DIST_DIR)/%.o: %.cc | $(DIST_DIR)
	@echo Compiling file "$<" ...
	@$(CXX) $(CFLAGS) -c $< -o $@

$(DIST_DIR):
	@mkdir $(DIST_DIR)

clean:
	@node -e "const fs=require('fs'),path=require('path'),dir='$(DIST_DIR)';if(fs.existsSync(dir))for(const file of fs.readdirSync(dir))if(['.js','.wasm','.a','.o','.d'].includes(path.extname(file)))fs.rmSync(path.join(dir,file))"

-include $(DEP_FILES)

.PHONY: all clean
