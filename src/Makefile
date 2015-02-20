#Makefile by Ketor 
#edited by Julio
TEST = test
BUILD = build
LIB = lib

CC        = gcc -D__USE_FILE_OFFSET64 -DHAVE_CONFIG_H -I. -D__CEPH__ -D_FILE_OFFSET_BITS=64 -D_REENTRANT -D_THREAD_SAFE -D__STDC_FORMAT_MACROS -D_GNU_SOURCE -fno-strict-aliasing -fsigned-char -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free -g -DPIC 
CPP       = g++ -D__USE_FILE_OFFSET64 -DHAVE_CONFIG_H -I. -D__CEPH__ -D_FILE_OFFSET_BITS=64 -D_REENTRANT -D_THREAD_SAFE -D__STDC_FORMAT_MACROS -D_GNU_SOURCE -fno-strict-aliasing -fsigned-char -Wno-invalid-offsetof -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free -g -DPIC 

PTHREAD=pthreadgc2

# Change the following directories according to your environment
INCLUDE_BASE=C:/MinGW
BOOST_INCLUDE_PATH=$(INCLUDE_BASE)/boost
PTHREADS_PATH=$(INCLUDE_BASE)/pthread

CEPH_INCLUDE = -I./ -I./global 
CFLAGS   = $(CEPH_INCLUDE)
CLIBS    = 

../$(BUILD)/%.o:%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@
../$(BUILD)/%.o:auth/%.cc
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



OBJECTS= ../$(BUILD)/MonClient.o ../$(BUILD)/assert.o ../$(BUILD)/ceph_context.o \
../$(BUILD)/Messenger.o ../$(BUILD)/errno.o ../$(BUILD)/types.o ../$(BUILD)/snap_types.o \
../$(BUILD)/osd_types.o ../$(BUILD)/Timer.o ../$(BUILD)/buffer.o ../$(BUILD)/Mutex.o \
../$(BUILD)/lockdep.o ../$(BUILD)/perf_counters.o ../$(BUILD)/Clock.o ../$(BUILD)/armor.o \
../$(BUILD)/environment.o ../$(BUILD)/safe_io.o ../$(BUILD)/strtol.o ../$(BUILD)/simple_spin.o \
../$(BUILD)/page.o ../$(BUILD)/sctp_crc32.o ../$(BUILD)/crc32c.o ../$(BUILD)/config.o \
../$(BUILD)/Thread.o ../$(BUILD)/code_environment.o ../$(BUILD)/io_priority.o ../$(BUILD)/signal.o \
../$(BUILD)/ConfUtils.o ../$(BUILD)/utf8.o ../$(BUILD)/ceph_argparse.o ../$(BUILD)/entity_name.o \
../$(BUILD)/msg_types.o 


../bin/rados.dll:$(OBJECTS)
	$(CPP) $(CFLAGS) $(CLIBS) -shared -o $@ $^ -lws2_32 -lpthreadGCE2
	@echo "**************************************************************"
	@echo "MAKE "$@" FINISH"
	@echo "**************************************************************"

../bin/ceph-dokan.exe:../$(BUILD)/rados_client.o ../$(BUILD)/librados.o ../$(BUILD)/RadosXattrIter.o ../$(BUILD)/snap_set_diff.o ../$(BUILD)/RadosClient.o ../$(BUILD)/IoCtxImpl.o  ../bin/rados.dll
	$(CPP) $(CFLAGS) $(CLIBS) -o $@ $^ -unicode 
	@echo "**************************************************************"
	@echo "MAKE "$@" FINISH"
	@echo "**************************************************************"


clean:
	rm -f $(OBJECTS) ../$(BUILD)/*.o ../bin/rados.dll ../bin/librados_client.exe
