GRPC_BUILD = $(PWD)/grpc_build
PB2_BUILD = $(PWD)/pb2_build

GRPC_PATH = $(GRPC_BUILD)/bins/opt

PROTOBUF3_PATH = $(GRPC_BUILD)/bins/opt/protobuf/
PROTOBUF2_PATH = $(PB2_BUILD)/bin/

#PROTOBUF_LIBPATH = $(GRPC_BUILD)/libs/opt/protobuf
PROTOBUF_LIBPATH = $(PB2_BUILD)/lib/

#PROTOBUF_INCPATH = $(GRPC_BUILD)/third_party/protobuf/src
PROTOBUF_INCPATH = $(PB2_BUILD)/include/

CXX = g++ #-fsanitize=address
CXXFLAGS = -g -std=gnu++11 -I. -I$(GRPC_BUILD)/include -I$(PROTOBUF_INCPATH) -Iproto
LDFLAGS = -pthread -L$(GRPC_BUILD)/libs/opt -L$(PROTOBUF_LIBPATH) -lprotobuf -lgrpc++ -lgrpc -lgpr

GRPC_RUN_LIBS = $(GRPC_BUILD)/libs/opt:$(PROTOBUF_LIBPATH)

PROTOC2 = PATH=$(PROTOBUF2_PATH):$(PATH) protoc
PROTOC3 = PATH=$(PROTOBUF3_PATH):$(PATH) protoc

GRPC_CPP_PLUGIN = grpc_cpp_plugin
GRPC_CPP_PLUGIN_PATH ?= `env PATH=$(GRPC_PATH):$$PATH which $(GRPC_CPP_PLUGIN)`

PROTOS_PATH = .

BINS = httpapi_test httpapi_test_async httpapi_test_callback
#BIN = httpapi_test
BIN = httpapi_test_async
#BIN = httpapi_test_callback

default: $(BINS)

#vpath %.proto $(PROTOS_PATH)
#

PROTO_OBJS = proto/httpapi_msg.pb.o proto/httpapi.grpc.pb.o

proto/httpapi.grpc.pb.cc: proto/httpapi.proto
	cd proto; $(PROTOC3) -I . --grpc_out=. --plugin=protoc-gen-grpc=$(GRPC_CPP_PLUGIN_PATH) httpapi.proto

proto/httpapi_msg.pb.cc: proto/httpapi_msg.proto
	cd proto; $(PROTOC2) -I . --cpp_out=. httpapi_msg.proto 
	ln -sf httpapi_msg.pb.h proto/httpapi.pb.h

clean:
	rm -f *.o  *.pb.* */*.pb.*

httpapi_test: $(PROTO_OBJS) test.o
	$(CXX) $^ $(LDFLAGS) -o $@

httpapi_test_async: $(PROTO_OBJS) test_async.o
	$(CXX) $^ $(LDFLAGS) -o $@

httpapi_test_callback: $(PROTO_OBJS) test_callback.o
	$(CXX) $^ $(LDFLAGS) -o $@

GRPC_TRACE_CLIENT=#executor,flowctl,queue_pluck,http,http2_stream_state
GRPC_TRACE_SERVER=#executor,flowctl,queue_pluck,http,http2_stream_state

RUNENV = GRPC_VERBOSITY=DEBUG
run: $(BINS) 
	$(RUNENV) LD_LIBRARY_PATH=$(GRPC_RUN_LIBS) ./$(BIN) both

run2: $(BINS) 
	trap "pkill 'httpapi_test_.*'" EXIT; $(RUNENV) GRPC_TRACE=$(GRPC_TRACE_SERVER) LD_LIBRARY_PATH=$(GRPC_RUN_LIBS) ./$(BIN) server &>/dev/null & sleep 0.1s; $(RUNENV) GRPC_TRACE=$(GRPC_TRACE_CLIENT) LD_LIBRARY_PATH=$(GRPC_RUN_LIBS) ./$(BIN) client
