MAKEFLAGS += -s -j

DIST_DIR = ./dist
SRC_DIR = ./deps/leveldb-mcpe

CXX = em++
CC = emcc

SRC_DIRS = $(SRC_DIR) $(wildcard $(SRC_DIR)/*/)

CFLAGS = -std=c++11 -O3 -pthread -I./deps/leveldb-mcpe -I./deps/leveldb-mcpe/include 
CFLAGS += -Wall -Wformat -Wno-unused-variable -Wno-attributes -Wno-sign-compare 
CFLAGS += -DDLLX= -DLEVELDB_PLATFORM_POSIX

LFLAGS = -lm -lnoderawfs.js -lnodefs.js -flto
LFLAGS += -s USE_ZLIB=1\
	-s INITIAL_MEMORY=128MB\
	-s TOTAL_STACK=32MB\
	-s SINGLE_FILE=1\
	-s WASM=1\
	-s MODULARIZE=1\
	-s EXPORT_NAME='LevelDB'\
	-s INVOKE_RUN=0\
	-s EXPORTED_RUNTIME_METHODS=FS,ccall,cwrap,getValue,setValue,UTF8ToString\
	-s FORCE_FILESYSTEM=1\
	-s NODERAWFS=1
LFLAGS += -s EXPORTED_FUNCTIONS="[\
	'_malloc',\
	'_free',\
	'_leveldb_open',\
	'_leveldb_close',\
	'_leveldb_put',\
	'_leveldb_delete',\
	'_leveldb_write',\
	'_leveldb_get',\
	'_leveldb_create_iterator',\
	'_leveldb_create_snapshot',\
	'_leveldb_release_snapshot',\
	'_leveldb_property_value',\
	'_leveldb_approximate_sizes',\
	'_leveldb_compact_range',\
	'_leveldb_destroy_db',\
	'_leveldb_repair_db',\
	'_leveldb_iter_destroy',\
	'_leveldb_iter_valid',\
	'_leveldb_iter_seek_to_first',\
	'_leveldb_iter_seek_to_last',\
	'_leveldb_iter_seek',\
	'_leveldb_iter_next',\
	'_leveldb_iter_prev',\
	'_leveldb_iter_key',\
	'_leveldb_iter_value',\
	'_leveldb_iter_get_error',\
	'_leveldb_writebatch_create',\
	'_leveldb_writebatch_destroy',\
	'_leveldb_writebatch_clear',\
	'_leveldb_writebatch_put',\
	'_leveldb_writebatch_delete',\
	'_leveldb_writebatch_iterate',\
	'_leveldb_options_create',\
	'_leveldb_options_destroy',\
	'_leveldb_options_set_comparator',\
	'_leveldb_options_set_filter_policy',\
	'_leveldb_options_set_create_if_missing',\
	'_leveldb_options_set_error_if_exists',\
	'_leveldb_options_set_paranoid_checks',\
	'_leveldb_options_set_env',\
	'_leveldb_options_set_info_log',\
	'_leveldb_options_set_write_buffer_size',\
	'_leveldb_options_set_max_open_files',\
	'_leveldb_options_set_cache',\
	'_leveldb_options_set_block_size',\
	'_leveldb_options_set_block_restart_interval',\
	'_leveldb_options_set_compression',\
	'_leveldb_comparator_create',\
	'_leveldb_comparator_destroy',\
	'_leveldb_filterpolicy_create',\
	'_leveldb_filterpolicy_destroy',\
	'_leveldb_filterpolicy_create_bloom',\
	'_leveldb_readoptions_create',\
	'_leveldb_readoptions_destroy',\
	'_leveldb_readoptions_set_verify_checksums',\
	'_leveldb_readoptions_set_fill_cache',\
	'_leveldb_readoptions_set_snapshot',\
	'_leveldb_writeoptions_create',\
	'_leveldb_writeoptions_destroy',\
	'_leveldb_writeoptions_set_sync',\
	'_leveldb_cache_create_lru',\
	'_leveldb_cache_destroy',\
	'_leveldb_create_default_env',\
	'_leveldb_env_destroy',\
	'_leveldb_free',\
	'_leveldb_major_version',\
	'_leveldb_minor_version'\
]"

CPP_SRC = db/builder.cc\
	db/c.cc\
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

vpath %.cc $(SRC_DIRS)

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