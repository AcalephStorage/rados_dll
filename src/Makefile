#Makefile by Ketor 
#edited by Julio
CC        = gcc -D__USE_FILE_OFFSET64 -DHAVE_CONFIG_H -I. -D__CEPH__ -D_FILE_OFFSET_BITS=64 -D_REENTRANT -D_THREAD_SAFE -D__STDC_FORMAT_MACROS -D_GNU_SOURCE -fno-strict-aliasing -fsigned-char -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free -g -DPIC 
CPP       = g++ -D__USE_FILE_OFFSET64 -DHAVE_CONFIG_H -I. -D__CEPH__ -D_FILE_OFFSET_BITS=64 -D_REENTRANT -D_THREAD_SAFE -D__STDC_FORMAT_MACROS -D_GNU_SOURCE -fno-strict-aliasing -fsigned-char -Wno-invalid-offsetof -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free -g -DPIC 

CEPH_INCLUDE = -I./ -I./global
BUILD = build
CFLAGS   = $(CEPH_INCLUDE)
CLIBS    = 

../$(BUILD)/%.o:%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@

../$(BUILD)/%.o:global/%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@

../$(BUILD)/%.o:auth/%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@

../$(BUILD)/%.o:auth/cephx/%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@

../$(BUILD)/%.o:auth/none/%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@

../$(BUILD)/%.o:include/%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@

../$(BUILD)/%.o:common/%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@

../$(BUILD)/%.o:crush/%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@

../$(BUILD)/%.o:mon/%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@

../$(BUILD)/%.o:osdc/%.cc
	$(CPP) -c $(CFLAGS) $^ -o $@

../$(BUILD)/%.o:common/%.c
	$(CC) -c $(CFLAGS) $^ -o $@

../$(BUILD)/%.o:crush/%.c
	$(CC) -c $(CFLAGS) $^ -o $@

../$(BUILD)/%.o:log/%.cc
	$(CC) -w -c $(CFLAGS) $^ -o $@

../$(BUILD)/%.o:msg/simple/%.cc
	$(CC) -w -c $(CFLAGS) $^ -o $@

../$(BUILD)/%.o:msg/%.cc
	$(CC) -w -c $(CFLAGS) $^ -o $@

../$(BUILD)/%.o:msg/async/%.cc
	$(CC) -w -c $(CFLAGS) $^ -o $@

../$(BUILD)/%.o:osd/%.cc
	$(CC) -w -c $(CFLAGS) $^ -o $@

../$(BUILD)/%.o:osdc/%.cc
	$(CC) -w -c $(CFLAGS) $^ -o $@

../$(BUILD)/%.o:os/%.cc
	$(CC) -w -c $(CFLAGS) $^ -o $@

../$(BUILD)/%.o:mds/%.cc
	$(CC) -w -c $(CFLAGS) $^ -o $@

../$(BUILD)/%.o:client/%.cc
	$(CC) -w -c $(CFLAGS) $^ -o $@

../$(BUILD)/%.o:librados/%.cc
	$(CC) -w -c $(CFLAGS) $^ -o $@

ALL:../bin/librados_client.exe

OBJECTS= ../$(BUILD)/global_context.o ../$(BUILD)/global_init.o ../$(BUILD)/pidfile.o ../$(BUILD)/signal_handler.o ../$(BUILD)/types.o ../$(BUILD)/io_priority.o ../$(BUILD)/hobject.o ../$(BUILD)/ceph_frag.o ../$(BUILD)/Readahead.o ../$(BUILD)/histogram.o ../$(BUILD)/uuid.o ../$(BUILD)/bloom_filter.o ../$(BUILD)/ceph_hash.o ../$(BUILD)/ceph_strings.o ../$(BUILD)/addr_parsing.o ../$(BUILD)/assert.o  ../$(BUILD)/BackTrace.o  ../$(BUILD)/buffer.o  ../$(BUILD)/ceph_argparse.o  ../$(BUILD)/ceph_context.o ../$(BUILD)/lockdep.o ../$(BUILD)/Clock.o ../$(BUILD)/ceph_crypto.o  ../$(BUILD)/code_environment.o  ../$(BUILD)/common_init.o  ../$(BUILD)/ConfUtils.o  ../$(BUILD)/DecayCounter.o  ../$(BUILD)/dout.o  ../$(BUILD)/entity_name.o  ../$(BUILD)/environment.o  ../$(BUILD)/errno.o  ../$(BUILD)/fd.o  ../$(BUILD)/Finisher.o  ../$(BUILD)/Formatter.o  ../$(BUILD)/hex.o ../$(BUILD)/LogEntry.o  ../$(BUILD)/Mutex.o  ../$(BUILD)/page.o  ../$(BUILD)/perf_counters.o  ../$(BUILD)/PrebufferedStreambuf.o  ../$(BUILD)/RefCountedObj.o    ../$(BUILD)/signal.o  ../$(BUILD)/simple_spin.o  ../$(BUILD)/snap_types.o  ../$(BUILD)/str_list.o  ../$(BUILD)/strtol.o  ../$(BUILD)/Thread.o  ../$(BUILD)/Throttle.o  ../$(BUILD)/Timer.o  ../$(BUILD)/util.o  ../$(BUILD)/config.o  ../$(BUILD)/armor.o ../$(BUILD)/crc32c.o ../$(BUILD)/crc32c-intel.o ../$(BUILD)/TrackedOp.o ../$(BUILD)/escape.o ../$(BUILD)/mime.o ../$(BUILD)/safe_io.o ../$(BUILD)/sctp_crc32.o ../$(BUILD)/secret.o ../$(BUILD)/utf8.o ../$(BUILD)/LogClient.o ../$(BUILD)/version.o ../$(BUILD)/Log.o  ../$(BUILD)/SubsystemMap.o ../$(BUILD)/Log.o  ../$(BUILD)/SubsystemMap.o ../$(BUILD)/AuthAuthorizeHandler.o ../$(BUILD)/AuthClientHandler.o ../$(BUILD)/AuthMethodList.o ../$(BUILD)/AuthServiceHandler.o ../$(BUILD)/AuthSessionHandler.o ../$(BUILD)/Crypto.o ../$(BUILD)/KeyRing.o ../$(BUILD)/RotatingKeyRing.o ../$(BUILD)/AuthNoneAuthorizeHandler.o ../$(BUILD)/CephxSessionHandler.o ../$(BUILD)/CephxAuthorizeHandler.o ../$(BUILD)/CephxProtocol.o   ../$(BUILD)/CephxClientHandler.o ../$(BUILD)/CephxServiceHandler.o ../$(BUILD)/CephxKeyServer.o  ../$(BUILD)/CrushCompiler.o ../$(BUILD)/CrushWrapper.o ../$(BUILD)/builder.o ../$(BUILD)/crush.o ../$(BUILD)/hash.o ../$(BUILD)/mapper.o ../$(BUILD)/hobject.o ../$(BUILD)/Accepter.o ../$(BUILD)/PipeConnection.o ../$(BUILD)/DispatchQueue.o ../$(BUILD)/Message.o ../$(BUILD)/Messenger.o ../$(BUILD)/msg_types.o ../$(BUILD)/Pipe.o ../$(BUILD)/SimpleMessenger.o ../$(BUILD)/HitSet.o ../$(BUILD)/OSDMap.o ../$(BUILD)/OpRequest.o ../$(BUILD)/osd_types.o ../$(BUILD)/MonClient.o ../$(BUILD)/MonMap.o ../$(BUILD)/MonCap.o ../$(BUILD)/flock.o ../$(BUILD)/MDSMap.o ../$(BUILD)/mdstypes.o ../$(BUILD)/inode_backtrace.o ../$(BUILD)/Filer.o ../$(BUILD)/Journaler.o ../$(BUILD)/ObjectCacher.o ../$(BUILD)/Objecter.o ../$(BUILD)/Striper.o ../$(BUILD)/Client.o ../$(BUILD)/ClientSnapRealm.o ../$(BUILD)/Dentry.o ../$(BUILD)/Inode.o ../$(BUILD)/MetaRequest.o ../$(BUILD)/MetaSession.o ../$(BUILD)/Trace.o ../$(BUILD)/uuid.o

../bin/rados.dll:$(OBJECTS)
	$(CPP) $(CFLAGS) $(CLIBS) -shared -o $@ $^ -lws2_32 -lpthreadGCE2
	@echo "**************************************************************"
	@echo "MAKE "$@" FINISH"
	@echo "**************************************************************"

../bin/librados_client.exe:../$(BUILD)/IoCtxImpl.o ../$(BUILD)/librados_client.o ../$(BUILD)/RadosXattrIter.o ../$(BUILD)/snap_set_diff.o ../bin/rados.dll
	$(CPP) $(CFLAGS) $(CLIBS) -o $@ $^ -lws2_32 -lpthreadGCE2 -unicode 
	@echo "**************************************************************"
	@echo "MAKE "$@" FINISH"
	@echo "**************************************************************"

clean:
	rm -f $(OBJECTS) ../$(BUILD)/*.o ../bin/rados.dll ../bin/librados_client.exe

