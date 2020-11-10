#include <unistd.h>
#include <future>
#include <grpcpp/grpcpp.h>
#include <grpc/support/time.h>
#include "httpapi.pb.h"
#include "httpapi.grpc.pb.h"

using namespace std;
using namespace httpapi;
using namespace grpc;

size_t blockSize = 2 << 20;
int maxMessageSize = 512 << 20;

class HttpApiException : public std::exception
{
public:
    HttpApiException(int code, const string & msg):
        mCode(code), mMsg(msg)
    {
    }

private:
    int mCode;
    string mMsg;
};

class HttpApiClient
{
public:
    HttpApiClient(std::shared_ptr<ChannelInterface> channel) : mChannel(channel)
    {
    }

    std::future<HttpApiResponse> Request(const string& method, const string& uri, const string& body)
    {
        std::promise<HttpApiResponse> p;

        auto stub = HttpApi::NewStub(mChannel);
        //CompletionQueue cq;
        grpc::ClientContext ctx;

        auto stream = stub->DoRequest(&ctx);

        HttpApiRequest req;
        req.set_method(method);
        req.set_uri(uri);

        size_t chunkSize = std::min(body.length(), blockSize);
        {
            const char *p = body.c_str();
            req.set_body_chunk(p, chunkSize); 
            stream->Write(req);

            size_t i = chunkSize;
            while (i < body.length())
            {
                HttpApiRequest req2;
                req2.set_body_chunk(p + i, std::min(blockSize, body.length() - i));
                i += blockSize;
                stream->Write(req2);
            }
        }
        stream->WritesDone();

        HttpApiResponse resp;
        HttpApiResponse respTmp;
        string respBody;

        bool first = true;
        while (stream->Read(&respTmp))
        {
            if (first)
            {
                first = false;
                resp.set_status(respTmp.status());
                respBody = respTmp.body_chunk();
            }
            else
            {
                respBody += respTmp.body_chunk();
            }
        }
        Status rpcStatus = stream->Finish();

        if (!rpcStatus.ok())
        {
            cerr << "rpc status not ok: " << rpcStatus.error_code() << ", " << rpcStatus.error_message() << endl;
            p.set_exception(std::make_exception_ptr(HttpApiException(rpcStatus.error_code(), rpcStatus.error_message())));
        }

        resp.set_body(respBody);

        p.set_value(resp);
        return p.get_future();
    }
private:
    //string mAddress;
    std::shared_ptr<ChannelInterface> mChannel;
};

class HttpApiServiceImpl : public HttpApi::Service
{
public:
    Status DoRequest(ServerContext* context, ServerReaderWriter<HttpApiResponse, HttpApiRequest>* stream) override
    {
        auto st = gpr_now(GPR_CLOCK_REALTIME);
        string body;
        HttpApiResponse resp;

        HttpApiRequest req;
        HttpApiRequest reqTmp;
        bool first = true;
        while (stream->Read(&reqTmp))
        {
            if (first)
            {
                first = false;
                resp.set_status(200);
                if (!reqTmp.body().empty())
                {
                    body = reqTmp.body();
                }
                else
                {
                    body = reqTmp.body_chunk();
                }
                std::swap(req, reqTmp);
            }
            else
            {
                body += reqTmp.body_chunk();
            }
        }

        if (req.uri() != "/echo")
        {
            cout << "Received request: " << req.method() << " " << req.uri() << " Size: " << body.size() << endl;
        }

        if (body.length() <= blockSize)
        {
            resp.set_body(body);
            stream->Write(resp);
        }
        else
        {
            const char *p = body.c_str();
            resp.set_body_chunk(p, blockSize); 
            stream->Write(resp);

            size_t i = blockSize;
            while (i < body.length())
            {
                HttpApiResponse resp2;
                resp2.set_body_chunk(p + i, std::min(blockSize, body.length() - i));
                i += blockSize;
                stream->Write(resp2);
            }
        }

        auto et = gpr_now(GPR_CLOCK_REALTIME);
        auto t = gpr_time_to_millis(gpr_time_sub(et, st));
        if (t > 1)
        {
            cout << "Slow serve(ms): " << t << " uri: " << req.uri() << endl;
        }
        return Status::OK;
    }
};

class HttpApiServer
{
public:
    HttpApiServer(const string& address) : mAddress(address)
    {
    }

    void Start()
    {
        ServerBuilder builder;
        builder.SetMaxReceiveMessageSize(maxMessageSize);
        builder.SetMaxSendMessageSize(maxMessageSize);
        builder.SetSyncServerOption(ServerBuilder::NUM_CQS, 3);
        builder.SetSyncServerOption(ServerBuilder::MAX_POLLERS, 10);
        builder.SetSyncServerOption(ServerBuilder::CQ_TIMEOUT_MSEC, 1000);
        builder.AddListeningPort(mAddress, grpc::InsecureServerCredentials());
        builder.RegisterService(&mService);
        mServer = builder.BuildAndStart();
        std::cout << "Server listening on " << mAddress << std::endl;
    }

    void Run()
    {
        Start();
        mServer->Wait();
    }

private:
    string mAddress;
    HttpApiServiceImpl mService;
    std::unique_ptr<Server> mServer;
};

void SendLargeRequest(std::shared_ptr<ChannelInterface> address)
{
    while (true)
    {
        sleep(3);

        string msg(500 << 20, 'a');

        cout << "LargeRequestStart" << endl;

        HttpApiClient client(address);

        auto st = gpr_now(GPR_CLOCK_REALTIME);
        std::future<HttpApiResponse> reply = client.Request("GET", "/large", msg);
        HttpApiResponse resp = reply.get();
        auto et = gpr_now(GPR_CLOCK_REALTIME);
        auto t = gpr_time_to_millis(gpr_time_sub(et, st));
        cout << "LargeRequestTime(ms): " << t << " Size: " << resp.body().size() << endl;
    }
}

void SendSmallRequest(std::shared_ptr<ChannelInterface> address)
{
    HttpApiClient client(address);
    while (true)
    {
        auto st = gpr_now(GPR_CLOCK_REALTIME);
        std::future<HttpApiResponse> reply = client.Request("GET", "/echo", "hi");
        HttpApiResponse resp = reply.get();
        auto et = gpr_now(GPR_CLOCK_REALTIME);
        auto t = gpr_time_to_millis(gpr_time_sub(et, st));
        if (t > 1)
        {
            cout << "slow rpc(ms): " << t << endl;
        }
        if (resp.status() != 200 || resp.body() != "hi")
        {
            cerr << "Bad response! status: " << resp.status() << "body: " << resp.body() << endl;
            exit(1);
        }
        //cout << "Response: " << resp.status() << ": " << resp.body() << endl;
        usleep(10 * 1000);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        cerr << "args: <client|server|both>" << endl;
    }

    string type = argv[1];

    try
    {
        string address = "localhost:8123";

        HttpApiServer server(address);

        if (type == "server")
        {
            server.Run();
        }
        else if (type == "both")
        {
            server.Start();
        }

        if (type != "server")
        {
            ChannelArguments args;
            args.SetMaxSendMessageSize(maxMessageSize);
            args.SetMaxReceiveMessageSize(maxMessageSize);
            auto channel = grpc::CreateCustomChannel(address, grpc::InsecureChannelCredentials(), args);

            //std::thread t(SendLargeRequest, channel);
            //std::thread tt(SendLargeRequest, channel);

            std::thread t1(SendSmallRequest, channel);
            //std::thread t2(SendSmallRequest, channel);
            //std::thread t3(SendSmallRequest, channel);

            t1.join();
        }
    }
    catch (std::exception& e)
    {
        cerr << e.what() << endl;
    }
}
