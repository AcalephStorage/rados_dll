#edited by Julio
TEST = test
BUILD = build
BIN = bin
LIB = lib

CC        = gcc -lws2_32 -D__USE_FILE_OFFSET64 -DHAVE_CONFIG_H -I. -D__CEPH__ -D_FILE_OFFSET_BITS=64 -D_REENTRANT -D_THREAD_SAFE -D__STDC_FORMAT_MACROS -D_GNU_SOURCE -fno-strict-aliasing -fsigned-char -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free -g -DPIC
CPP       = g++ -lws2_32 -D__USE_FILE_OFFSET64 -DHAVE_CONFIG_H -I. -D__CEPH__ -D_FILE_OFFSET_BITS=64 -D_REENTRANT -D_THREAD_SAFE -D__STDC_FORMAT_MACROS -D_GNU_SOURCE -fno-strict-aliasing -fsigned-char -Wno-invalid-offsetof -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free -g -DPIC

PTHREAD=pthreadgce2

# Change the following directories according to your environment
INCLUDE_BASE=C:/MinGW
BOOST_INCLUDE_PATH=$(INCLUDE_BASE)/boost
PTHREADS_PATH=$(INCLUDE_BASE)/pthread

CEPH_INCLUDE = -I./ -I./global -lpthreadGCE2
CFLAGS   = $(CEPH_INCLUDE)
CLIBS    = 

../$(BUILD)/%.o:%.c
	$(CC) -c $(CFLAGS) $^ -o $@
../$(BUILD)/%.o:%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@
../$(BUILD)/%.o:auth/%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@
../$(BUILD)/%.o:auth/cephx/%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@	
../$(BUILD)/%.o:mon/%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@
../$(BUILD)/%.o:cls/%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@
../$(BUILD)/%.o:include/%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@
../$(BUILD)/%.o:common/%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@
../$(BUILD)/%.o:common/%.c
	$(CC) -c $(CFLAGS) $^ -o $@
../$(BUILD)/%.o:librados/%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@
../$(BUILD)/%.o:log/%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@
../$(BUILD)/%.o:osdc/%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@
../$(BUILD)/%.o:osd/%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@
../$(BUILD)/%.o:msg/%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@
../$(BUILD)/%.o:%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@
../$(BUILD)/%.o:cls/%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@
../$(BUILD)/%.o:mds/%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@
../$(BUILD)/%.o:mon/%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@
../$(BUILD)/%.o:global/%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@
../$(BUILD)/%.o:crush/%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@
../$(BUILD)/%.o:crush/%.c
	$(CC) -c $(CFLAGS) $^ -o $@
../$(BUILD)/%.o:cls/lock/%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@
../$(BUILD)/%.o:mds/%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@
../$(BUILD)/%.o:msg/simple/%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@
../$(BUILD)/%.o:json_spirit/%.cpp
	$(CPP) -c $(CFLAGS) $^ -o $@

OBJECTS= ../$(BUILD)/hash.o ../$(BUILD)/snap_set_diff.o ../$(BUILD)/librados.o ../$(BUILD)/IoCtxImpl.o ../$(BUILD)/RadosClient.o ../$(BUILD)/RadosXattrIter.o \
../$(BUILD)/Objecter.o ../$(BUILD)/MonClient.o ../$(BUILD)/CrushWrapper.o  ../$(BUILD)/MonMap.o \
  ../$(BUILD)/mdstypes.o ../$(BUILD)/assert.o ../$(BUILD)/Pipe.o ../$(BUILD)/admin_socket.o \
 ../$(BUILD)/MDSMap.o ../$(BUILD)/ceph_argparse.o ../$(BUILD)/OSDMap.o ../$(BUILD)/admin_socket_client.o \
../$(BUILD)/entity_name.o  ../$(BUILD)/ceph_context.o ../$(BUILD)/CephxClientHandler.o \
../$(BUILD)/Message.o ../$(BUILD)/common_init.o ../$(BUILD)/osd_types.o ../$(BUILD)/Messenger.o \
../$(BUILD)/RotatingKeyRing.o ../$(BUILD)/CephxProtocol.o ../$(BUILD)/CephxSessionHandler.o ../$(BUILD)/global_context.o \
../$(BUILD)/LogClient.o  ../$(BUILD)/AuthMethodList.o ../$(BUILD)/ceph_strings.o \
../$(BUILD)/Formatter.o  ../$(BUILD)/escape.o ../$(BUILD)/ceph_crypto.o ../$(BUILD)/builder.o \
../$(BUILD)/Timer.o  ../$(BUILD)/BackTrace.o ../$(BUILD)/hex.o ../$(BUILD)/crush.o \
../$(BUILD)/Finisher.o ../$(BUILD)/Throttle.o ../$(BUILD)/ceph_fs.o ../$(BUILD)/mapper.o \
../$(BUILD)/ceph_frag.o ../$(BUILD)/config.o ../$(BUILD)/MonCap.o  ../$(BUILD)/Filer.o \
../$(BUILD)/types.o  ../$(BUILD)/Crypto.o ../$(BUILD)/str_map.o ../$(BUILD)/json_spirit_reader.o ../$(BUILD)/json_spirit_writer.o \
../$(BUILD)/errno.o  ../$(BUILD)/snap_types.o ../$(BUILD)/Striper.o  \
../$(BUILD)/buffer.o ../$(BUILD)/Mutex.o ../$(BUILD)/hash.o ../$(BUILD)/ObjectCacher.o \
../$(BUILD)/lockdep.o ../$(BUILD)/perf_counters.o ../$(BUILD)/Clock.o \
../$(BUILD)/armor.o ../$(BUILD)/AuthClientHandler.o ../$(BUILD)/LogEntry.o \
../$(BUILD)/environment.o ../$(BUILD)/safe_io.o ../$(BUILD)/strtol.o \
../$(BUILD)/simple_spin.o  ../$(BUILD)/Clock.o ../$(BUILD)/Journaler.o \
../$(BUILD)/page.o ../$(BUILD)/sctp_crc32.o ../$(BUILD)/crc32c.o ../$(BUILD)/cmdparse.o \
 ../$(BUILD)/KeyRing.o ../$(BUILD)/RefCountedObj.o ../$(BUILD)/str_list.o \
../$(BUILD)/Thread.o ../$(BUILD)/code_environment.o ../$(BUILD)/SimpleMessenger.o \
../$(BUILD)/io_priority.o ../$(BUILD)/signal.o ../$(BUILD)/cls_lock_client.o \
../$(BUILD)/ConfUtils.o ../$(BUILD)/utf8.o ../$(BUILD)/hobject.o \
../$(BUILD)/bloom_filter.o ../$(BUILD)/DecayCounter.o ../$(BUILD)/inode_backtrace.o \
../$(BUILD)/msg_types.o ../$(BUILD)/SubsystemMap.o ../$(BUILD)/uuid.o \
../$(BUILD)/HitSet.o  ../$(BUILD)/Log.o ../$(BUILD)/PrebufferedStreambuf.o ../$(BUILD)/version.o \
../$(BUILD)/Accepter.o ../$(BUILD)/DispatchQueue.o  ../$(BUILD)/PipeConnection.o ../$(BUILD)/dout.o \
../$(BUILD)/AuthSessionHandler.o ../$(BUILD)/histogram.o ../$(BUILD)/ceph_hash.o ../$(BUILD)/addr_parsing.o ../$(BUILD)/cmdparse.o

all: ../$(BIN)/rados_client.exe

../$(BIN)/rados.dll:$(OBJECTS)
	$(CPP) $(CFLAGS) $(CLIBS) -shared -o $@ $^ -lws2_32 -lpthreadGCE2 -lgio-2.0 -lglib-2.0 -lgobject-2.0 -lboost_system-mgw48-mt-1_57 -lboost_system-mgw48-mt-d-1_57 -lboost_atomic-mgw48-mt-1_57 -lboost_context-mgw48-mt-1_57 -lboost_context-mgw48-mt-d-1_57 -lboost_filesystem-mgw48-mt-1_57 -lboost_filesystem-mgw48-mt-d-1_57 -lboost_iostreams-mgw48-mt-1_57 -lboost_iostreams-mgw48-mt-d-1_57 -lboost_log-mgw48-mt-1_57 -lboost_log-mgw48-mt-d-1_57 -lboost_signals-mgw48-mt-1_57 -lboost_signals-mgw48-mt-d-1_57 -lboost_thread-mgw48-mt-1_57 -lboost_thread-mgw48-mt-d-1_57 -lboost_atomic-mgw48-mt-1_57 
	@echo "**************************************************************"
	@echo "MAKE "$@" FINISH"
	@echo "**************************************************************"

../$(BIN)/rados_client.exe:../$(BUILD)/rados_client.o ../$(BIN)/rados.dll
	$(CPP) $(CFLAGS) $(CLIBS) -o $@ $^ -unicode -lws2_32 -l$(PTHREAD) -lgio-2.0 -lglib-2.0 -lgobject-2.0 \
	-lboost_thread-mgw48-mt-1_57 -lboost_atomic-mgw48-mt-1_57 -lboost_log-mgw48-mt-1_57 -lboost_system-mgw48-mt-1_57
	@echo "**************************************************************"
	@echo "MAKE "$@" FINISH"
	@echo "**************************************************************"


clean:
	rm -f $(OBJECTS) ../$(BUILD)/*.o ../$(BIN)/rados.dll ../$(BIN)/rados_client.exe
