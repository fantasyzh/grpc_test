#include <unistd.h>
#include <sys/syscall.h>
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

class HttpApiClient : public grpc::experimental::ClientBidiReactor<httpapi::HttpApiRequest, httpapi::HttpApiResponse>
{
public:
    HttpApiClient(std::shared_ptr<ChannelInterface> channel) : mChannel(channel), mFirstRead(true), mWriteIdx(0)
    {
    }

    std::future<HttpApiResponse> Request(const string& method, const string& uri, const string& body)
    {
        auto stub = HttpApi::NewStub(mChannel);

        stub->experimental_async()->DoRequest(&mCtx, this);

        mReq.set_method(method);
        mReq.set_uri(uri);

        if (body.length() <= blockSize)
        {
            mReq.set_body(body);
            StartWrite(&mReq);
        }
        else
        {
            mReq.set_body_chunk(body.c_str(), blockSize); 
            StartWrite(&mReq);
            mWriteIdx = blockSize;
            mBody = body;
        }
        StartCall();
        return mPromise.get_future();
    }

    void OnReadDone(bool ok) override {
        cout << "OnReadDone" << " " << syscall(SYS_gettid) << endl;
        if (!ok) {
            cerr << "Client read failed" << endl;
            return;
        }

        if (mFirstRead)
        {
            mFirstRead = false;
            mResp.set_status(mRespTmp.status());
            if (!mRespTmp.body().empty())
            {
                mRespBody = mRespTmp.body();
            }
            else
            {
                mRespBody = mRespTmp.body_chunk();
                StartRead(&mRespTmp);
            }
        }
        else
        {
            mRespBody += mRespTmp.body_chunk();
            StartRead(&mRespTmp);
        }
    }

    void OnWriteDone(bool ok) override {
        cout << "OnWriteDone" << " " << syscall(SYS_gettid) << endl;
        if (!ok) {
            cerr << "Client write failed" << endl;
            return;
        }

        if (mWriteIdx == 0)
        {
            StartWritesDone();
            StartRead(&mRespTmp);
            return;
        }

        if (mWriteIdx < mBody.length())
        {
            size_t chunkSize = std::min(blockSize, mBody.length() - mWriteIdx);
            mReq2.set_body_chunk(mBody.c_str() + mWriteIdx, chunkSize);
            mWriteIdx += chunkSize;
            StartWrite(&mReq2);
        }
        else
        {
            StartWritesDone();
            StartRead(&mRespTmp);
        }
    }

    void OnDone(const Status& s) override {
        if (!s.ok()) {
            cerr << "client rpc failed" << endl;
            return;
        }

        mResp.set_body(mRespBody);
        mPromise.set_value(mResp);
    }

private:
    //string mAddress;
    std::shared_ptr<ChannelInterface> mChannel;
    grpc::ClientContext mCtx;
    bool mFirstRead;
    int mWriteIdx;
    string mBody;
    HttpApiRequest mReq;
    HttpApiRequest mReq2;
    HttpApiResponse mResp;
    HttpApiResponse mRespTmp;
    string mRespBody;
    std::promise<HttpApiResponse> mPromise;
};

class HttpApiServiceImpl : public HttpApi::ExperimentalCallbackService
{
public:
    experimental::ServerBidiReactor<HttpApiRequest, HttpApiResponse>* DoRequest() override
    {
        class Reactor
            : public experimental::ServerBidiReactor<HttpApiRequest, HttpApiResponse>
        {
        public:
            Reactor() : mFirstRead(true), mWriteIdx(0) {}
            void OnStarted(ServerContext* context) override {
                mCtx = context;
                mStartTime = gpr_now(GPR_CLOCK_REALTIME);
                StartRead(&mReq);
            }
            void OnDone() override {
                auto et = gpr_now(GPR_CLOCK_REALTIME);
                auto t = gpr_time_to_millis(gpr_time_sub(et, mStartTime));
                if (t > 1)
                {
                    cout << "Slow serve(ms): " << t << " uri: " << mReq.uri() << endl;
                }
                Finish(::grpc::Status::OK);
                delete this;
            }
            void OnCancel() override {}
            void OnReadDone(bool ok) override {
                if (!ok) {
                    if (mReq.uri() != "/echo")
                    {
                        cout << "Received request: " << mReq.method() << " " << mReq.uri() << " Size: " << mBody.size() << endl;
                    }
                    if (mBody.length() <= blockSize)
                    {
                        mResp.set_body(mBody);
                        StartWrite(&mResp);
                    }
                    else
                    {
                        mResp.set_body_chunk(mBody.c_str(), blockSize); 
                        StartWrite(&mResp);
                        mWriteIdx = blockSize;
                    }
                    return;
                }

                if (mFirstRead)
                {
                    mFirstRead = false;
                    mResp.set_status(200);
                    if (!mReqTmp.body().empty())
                    {
                        mBody = mReqTmp.body();
                    }
                    else
                    {
                        mBody = mReqTmp.body_chunk();
                    }
                    std::swap(mReq, mReqTmp);
                }
                else
                {
                    mBody += mReqTmp.body_chunk();
                }
            }
            void OnWriteDone(bool ok) override {
                if (!ok) {
                    cerr << "Server write failed" << endl;
                    return;
                }
                if (mWriteIdx == 0)
                {
                    return;
                }

                if (mWriteIdx < mBody.length())
                {
                    size_t chunkSize = std::min(blockSize, mBody.length() - mWriteIdx);
                    mResp2.set_body_chunk(mBody.c_str() + mWriteIdx, chunkSize);
                    mWriteIdx += chunkSize;
                    StartWrite(&mResp2);
                }
            }

        private:
            ServerContext *mCtx;
            gpr_timespec mStartTime;
            bool mFirstRead;
            string mBody;
            int mWriteIdx;
            HttpApiResponse mResp;
            HttpApiResponse mResp2;

            HttpApiRequest mReq;
            HttpApiRequest mReqTmp;
        };

        return new Reactor;
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

    shared_ptr<Channel> InProcessChannel(ChannelArguments& args) { return mServer->InProcessChannel(args); }

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
    while (true)
    {
        HttpApiClient client(address);
        auto st = gpr_now(GPR_CLOCK_REALTIME);
        std::future<HttpApiResponse> reply = client.Request("GET", "/echo2", "hi");
        HttpApiResponse resp = reply.get();
        auto et = gpr_now(GPR_CLOCK_REALTIME);
        auto t = gpr_time_to_millis(gpr_time_sub(et, st));
        if (t > 0)
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
            //auto channel = grpc::CreateCustomChannel(address, grpc::InsecureChannelCredentials(), args);
            auto channel = server.InProcessChannel(args);

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
