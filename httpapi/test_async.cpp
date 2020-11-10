#include <unistd.h>
#include <sys/syscall.h>
#include <future>
#include <sstream>
#include <grpcpp/grpcpp.h>
#include <grpc/support/time.h>
#include "httpapi.pb.h"
#include "httpapi.grpc.pb.h"

using namespace std;
using namespace httpapi;
using namespace grpc;

size_t blockSize = 1 << 20;
int maxMessageSize = 512 << 20;

std::atomic<uint64_t> qps;

class HttpApiException : public std::exception
{
public:
    HttpApiException(const Status& status):
        mStatus(status)
    {
    }

    const char *what() const noexcept override
    {
        return mStatus.error_message().c_str();
    }

private:
    Status mStatus;
};

typedef std::list<std::vector<char>> BufferList;

class HttpApiClient
{
public:
    int mOnStartTime;
    int mToStartTime;

    HttpApiClient(std::shared_ptr<Channel> channel) : mChannel(channel), mFirstRead(true), mWriteIdx(0), mRespBodySize(0),
        mReadTag(this, ClientTag::READ), 
        mWriteTag(this, ClientTag::WRITE),
        mStartTag(this, ClientTag::START),
        mWritesDoneTag(this, ClientTag::WRITES_DONE),
        mDoneTag(this, ClientTag::DONE)
    {
    }

    void SetTimeoutMs(int ms)
    {
        auto deadline = std::chrono::system_clock::now() +
                std::chrono::milliseconds(ms);
        mCtx.set_deadline(deadline);
    }

    std::future<HttpApiResponse> Request(const string& method, const string& uri, const string&& body)
    {
        InitOnce();

        auto stub = HttpApi::NewStub(mChannel);

        mReq.set_method(method);
        mReq.set_uri(uri);

        int chunkSize = std::min(blockSize, body.length());
        mReq.set_body_chunk(body.c_str(), chunkSize); 
        mWriteIdx = chunkSize;
        mBody = body;

        mToStartTime = gpr_now(GPR_CLOCK_REALTIME).tv_nsec;

        mStream = stub->PrepareAsyncDoRequest(&mCtx, sCq);

        mStream->StartCall((void*)&mStartTag);

        return mPromise.get_future();
    }

    void OnStarted(bool ok)
    {
        if (!ok)
        {
            mStream->Finish(&mRpcStatus, &mDoneTag);
        }
        mOnStartTime = gpr_now(GPR_CLOCK_REALTIME).tv_nsec;
        mStream->Write(mReq, &mWriteTag);
    }

    void OnReadDone(bool ok) {
        if (!ok) {
            {
                NameValuePair *header = mResp.mutable_headers()->Add();
                header->set_name("readDoneTime");
                char buf[20];
                sprintf(buf, "%09d", gpr_now(GPR_CLOCK_REALTIME).tv_nsec);
                header->set_value(buf);
            }
            mStream->Finish(&mRpcStatus, &mDoneTag);
            return;
        }

        if (mFirstRead)
        {
            mFirstRead = false;

            mResp.set_status(mRespTmp.status());
            (*mResp.mutable_headers()) = mRespTmp.headers();

            if (!mRespTmp.body().empty())
            {
                mRespBody = mRespTmp.body();
                mRespBodySize = mRespTmp.body().size();
                {
                    NameValuePair *header = mResp.mutable_headers()->Add();
                    header->set_name("readDoneTime");
                    char buf[20];
                    sprintf(buf, "%09d", gpr_now(GPR_CLOCK_REALTIME).tv_nsec);
                    header->set_value(buf);
                }
                mStream->Finish(&mRpcStatus, &mDoneTag);
            }
            else
            {
                //mRespBody = mRespTmp.body_chunk();
                mRespBodySize = mRespTmp.body_chunk().size();
                mStream->Read(&mRespTmp, &mReadTag);
            }
        }
        else
        {
            //mRespBody += mRespTmp.body_chunk();
            mRespBodySize += mRespTmp.body_chunk().size();
            mStream->Read(&mRespTmp, &mReadTag);
        }
    }

    void OnWriteDone(bool ok) {
        if (!ok) {
            cerr << "Client write failed" << endl;
            mStream->Finish(&mRpcStatus, &mDoneTag);
            return;
        }

        if (mWriteIdx < mBody.length())
        {
            size_t chunkSize = std::min(blockSize, mBody.length() - mWriteIdx);
            mReq2.set_body_chunk(mBody.c_str() + mWriteIdx, chunkSize);
            mWriteIdx += chunkSize;
            mStream->Write(mReq2, &mWriteTag);
        }
        else
        {
            mStream->WritesDone(&mWritesDoneTag);
        }
    }

    void OnWritesDone(bool ok)
    {
        if (!ok) {
            cerr << "Client writesdone failed" << endl;
            mStream->Finish(&mRpcStatus, &mDoneTag);
            return;
        }
        mStream->Read(&mRespTmp, &mReadTag);
    }

    void OnDone(const Status& s) {
        if (!s.ok()) {
            cerr << "client rpc failed" << endl;
            mPromise.set_exception(std::make_exception_ptr(HttpApiException(s)));
            return;
        }

        if (mReq.uri() == "/large")
        {
            mResp.set_status(mRespBodySize);
        }
        mResp.set_body(mRespBody);

        {
            NameValuePair *header = mResp.mutable_headers()->Add();
            header->set_name("serverFinishTime");
            auto v = mCtx.GetServerTrailingMetadata().find("server-finish-time")->second;
            header->set_value(string(v.data(), v.length()));
        }
        {
            NameValuePair *header = mResp.mutable_headers()->Add();
            header->set_name("doneTime");
            char buf[20];
            sprintf(buf, "%09d", gpr_now(GPR_CLOCK_REALTIME).tv_nsec);
            header->set_value(buf);
        }

        mPromise.set_value(mResp);
    }

private:
    static void InitOnce()
    {
        static std::once_flag sClientInitOnce;
        std::call_once(sClientInitOnce, &HttpApiClient::DoInit);
    }

    static void DoInit()
    {
        sCq = new CompletionQueue();
        for (int i = 0; i < NUM_POLLERS; i++)
        {
            sPoller[i] = std::thread(&HttpApiClient::PollerRun, sCq);
        }
    }

    struct ClientTag
    {
        HttpApiClient *c;
        enum TagType
        {
            READ,
            WRITE,
            START,
            WRITES_DONE,
            DONE
        } type;

        ClientTag(HttpApiClient *self, ClientTag::TagType tagType) : c(self), type(tagType) {}
    };

    static void PollerRun(CompletionQueue *cq)
    {
        void *gotTag;
        bool ok;
        while (cq->Next(&gotTag, &ok))
        {
            auto st = gpr_now(GPR_CLOCK_REALTIME);

            auto *tag = static_cast<HttpApiClient::ClientTag*>(gotTag);
            //cout << "tag: " << tag->type << " ok? " << ok << endl;
            HttpApiClient *c = tag->c;
            switch (tag->type)
            {
            case ClientTag::READ:
                c->OnReadDone(ok);
                break;
            case ClientTag::WRITE:
                c->OnWriteDone(ok);
                break;
            case ClientTag::DONE:
                c->OnDone(c->mRpcStatus);
                break;
            case ClientTag::START:
                c->OnStarted(ok);
                break;
            case ClientTag::WRITES_DONE:
                c->OnWritesDone(ok);
                break;
            default:
                cout << "Unknown tag" << endl;
                break;
            }

            auto et = gpr_now(GPR_CLOCK_REALTIME);
            auto t = gpr_time_to_millis(gpr_time_sub(et, st));
            if (t > 1)
            {
                gpr_log(GPR_DEBUG, "Poller Delay(ms): %d Since: %09d Client: %p", t, st.tv_nsec, c);
            }
        }
    }

    static CompletionQueue *sCq;
    static const int NUM_POLLERS = 3;
    static std::thread sPoller[NUM_POLLERS];

    //string mAddress;
    std::shared_ptr<Channel> mChannel;
    grpc::ClientContext mCtx;
    std::unique_ptr<ClientAsyncReaderWriter<HttpApiRequest, HttpApiResponse>> mStream;

    ClientTag mReadTag;
    ClientTag mWriteTag;
    ClientTag mStartTag;
    ClientTag mWritesDoneTag;
    ClientTag mDoneTag;

    bool mFirstRead;
    int mWriteIdx;
    string mBody;
    HttpApiRequest mReq;
    HttpApiRequest mReq2;
    HttpApiResponse mResp;
    HttpApiResponse mRespTmp;
    string mRespBody;
    size_t mRespBodySize;
    Status mRpcStatus;
    std::promise<HttpApiResponse> mPromise;
};

CompletionQueue *HttpApiClient::sCq;
std::thread HttpApiClient::sPoller[HttpApiClient::NUM_POLLERS];

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
        builder.AddChannelArgument("grpc.http2.lookahead_bytes", 10000000);
        builder.AddChannelArgument("grpc.http2.write_buffer_size", 10000000);
        builder.SetSyncServerOption(ServerBuilder::NUM_CQS, 3);
        builder.SetSyncServerOption(ServerBuilder::MAX_POLLERS, 10);
        builder.SetSyncServerOption(ServerBuilder::CQ_TIMEOUT_MSEC, 1000);
        builder.AddListeningPort(mAddress, grpc::InsecureServerCredentials());
        builder.RegisterService(&mService);
        mCq = builder.AddCompletionQueue();
        mServer = builder.BuildAndStart();
        std::cout << "Server listening on " << mAddress << std::endl;

        DoInit();
    }

    void Run()
    {
        Start();
        mServer->Wait();
    }

    shared_ptr<Channel> InProcessChannel(ChannelArguments& args) { return mServer->InProcessChannel(args); }

private:
    class CallData;
    struct ServerTag
    {
        CallData *c;
        enum TagType
        {
            READ,
            WRITE,
            START,
            DONE
        } type;

        ServerTag(CallData *c, ServerTag::TagType tagType) : c(c), type(tagType) {}
    };

    class CallData 
    {
    public:
        CallData(HttpApi::AsyncService *service, ServerCompletionQueue *cq):
            mService(service), mCq(cq), mStream(&mCtx),
            mReadTag(this, ServerTag::READ), mWriteTag(this, ServerTag::WRITE), mStartTag(this, ServerTag::START), mDoneTag(this, ServerTag::DONE),
            mFirstRead(true), mWriteIdx(0), mBodySize(0)
        {
            mService->RequestDoRequest(&mCtx, &mStream, mCq, mCq, (void*)&mStartTag);
        }

        void OnStarted(bool ok) {
            if (!ok)
            {
                cerr << "Server start rpc failed" << endl;
                return;
            }
            new CallData(mService, mCq);
            mStartTime = gpr_now(GPR_CLOCK_REALTIME);
            mStream.Read(&mReqTmp, &mReadTag);
        }

        void OnDone(bool ok) {
            if (!ok)
            {
                cerr << "Server finish failed" << endl;
                return;
            }
            auto et = gpr_now(GPR_CLOCK_REALTIME);
            auto t = gpr_time_to_millis(gpr_time_sub(et, mStartTime));
            if (t > 2)
            {
                cout << "Slow serve(ms): " << t << " uri: " << mReq.uri() << endl;
            }
            delete this;
        }

        void OnCancel() {}

        void StartReply()
        {
            if (mReq.uri() != "/echo")
            {
                cout << "Received request: " << mReq.method() << " " << mReq.uri() << " Size: " << mBodySize << endl;
            }

            if (mReq.uri() == "/large")
            {
                if (mReq.method() == "GET")
                {
                    //mBody.resize(500 << 20, 'a');
                    mBodySize = 500 << 20;
                }
                else
                {
                    mBody.clear();
                }
            }

            mResp.set_status(200);

            {
                char buf[20];
                sprintf(buf, "%09d", mStartTime.tv_nsec);
                NameValuePair *header = mResp.mutable_headers()->Add();
                header->set_name("serverStartTime");
                header->set_value(buf);
            }

            {
                NameValuePair *header = mResp.mutable_headers()->Add();
                header->set_name("serverWriteTime");
                char buf[20];
                sprintf(buf, "%09d", gpr_now(GPR_CLOCK_REALTIME).tv_nsec);
                header->set_value(buf);
            }

            if (mBodySize <= blockSize)
            {
                mResp.set_body(mBody);
                mStream.Write(mResp, &mWriteTag);
            }
            else
            {
                //mResp.set_body_chunk(mBody.c_str(), blockSize); 
                mResp.set_body_chunk(string(blockSize, 'a')); 
                mStream.Write(mResp, &mWriteTag);
                mWriteIdx = blockSize;
            }
        }

        void OnReadDone(bool ok) {
            if (!ok) {
                StartReply();
                return;
            }

            if (mFirstRead)
            {
                mFirstRead = false;
                std::swap(mReq, mReqTmp);
                if (!mReq.body().empty())
                {
                    mBody = mReq.body();
                    mBodySize = mReq.body().size();

                    // request processing
                    StartReply();
                    return;
                }
                else
                {
                    mBody = mReq.body_chunk();
                    mBodySize = mReq.body_chunk().size();
                }
            }
            else
            {
                //mBody += mReqTmp.body_chunk();
                mBodySize += mReqTmp.body_chunk().size();
            }
            mStream.Read(&mReqTmp, &mReadTag);
        }

        void OnWriteDone(bool ok) {
            if (!ok) {
                cerr << "Server write failed" << endl;
                return;
            }
            if (mWriteIdx == 0)
            {
                char buf[20];
                sprintf(buf, "%09d", gpr_now(GPR_CLOCK_REALTIME).tv_nsec);
                mCtx.AddTrailingMetadata("server-finish-time", buf);
                mStream.Finish(Status::OK, &mDoneTag);
                return;
            }

            if (mWriteIdx < mBodySize)
            {
                size_t chunkSize = std::min(blockSize, mBodySize - mWriteIdx);
                //mResp2.set_body_chunk(mBody.c_str() + mWriteIdx, chunkSize);
                mResp2.set_body_chunk(string(chunkSize, 'a'));
                mWriteIdx += chunkSize;
                mStream.Write(mResp2, &mWriteTag);
            }
            else
            {
                char buf[20];
                sprintf(buf, "%09d", gpr_now(GPR_CLOCK_REALTIME).tv_nsec);
                mCtx.AddTrailingMetadata("server-finish-time", buf);
                mStream.Finish(Status::OK, &mDoneTag);
                return;
            }
        }

    private:
        string mMethod;

        ServerContext mCtx;
        ServerAsyncReaderWriter<HttpApiResponse, HttpApiRequest> mStream;

        gpr_timespec mStartTime;
        bool mFirstRead;
        string mBody;
        size_t mBodySize;
        int mWriteIdx;
        HttpApiResponse mResp;
        HttpApiResponse mResp2;

        HttpApiRequest mReq;
        HttpApiRequest mReqTmp;

        HttpApi::AsyncService *mService;
        ServerCompletionQueue *mCq;

        ServerTag mReadTag;
        ServerTag mWriteTag;
        ServerTag mStartTag;
        ServerTag mDoneTag;

    };

    void DoInit()
    {
        for (int i = 0; i < NUM_POLLERS; i++)
        {
            mPoller[i] = std::thread(&HttpApiServer::PollerRun, mCq.get(), this);
        }
    }

    static void PollerRun(ServerCompletionQueue *cq, HttpApiServer *s)
    {
        new CallData(&s->mService, cq);

        void *gotTag;
        bool ok;
        while (cq->Next(&gotTag, &ok))
        {
            auto st = gpr_now(GPR_CLOCK_REALTIME);

            auto *tag = static_cast<HttpApiServer::ServerTag*>(gotTag);
            CallData *c = tag->c;
            //cout << "tag: " << (void*)c << " " << tag->type << " ok? " << ok << endl;
            switch (tag->type)
            {
            case ServerTag::READ:
                c->OnReadDone(ok);
                break;
            case ServerTag::WRITE:
                c->OnWriteDone(ok);
                break;
            case ServerTag::DONE:
                c->OnDone(ok);
                break;
            case ServerTag::START:
                c->OnStarted(ok);
                break;
            default:
                cout << "Unknown tag" << endl;
                break;
            }

            auto et = gpr_now(GPR_CLOCK_REALTIME);
            auto t = gpr_time_to_millis(gpr_time_sub(et, st));
            if (t > 1)
            {
                gpr_log(GPR_DEBUG, "Poller Delay(ms): %d Since: %09d CallData: %p", t, st.tv_nsec, c);
            }
        }
    }
    string mAddress;
    HttpApi::AsyncService mService;
    std::unique_ptr<Server> mServer;

    std::unique_ptr<ServerCompletionQueue> mCq;
    static const int NUM_POLLERS = 3;
    std::thread mPoller[NUM_POLLERS];
};

void SendLargeRequest(std::shared_ptr<Channel> address, const string& method, int sec)
{
    //class RequestPublisher : public Publisher
    //{
    //};

    //class ResponseStreamSubscriber : public Subscriber
    //{
    //};

    sleep(sec);
    while (true)
    {
        string msg(500 << 20, 'a');

        HttpApiClient client(address);
        client.SetTimeoutMs(800);

        gpr_log(GPR_DEBUG, "LargeRequestStart %s %p", method.c_str(), &client);

        try
        {
            auto st = gpr_now(GPR_CLOCK_REALTIME);
            std::future<HttpApiResponse> reply = client.Request(method, "/large", method == "GET"? "": msg);
            HttpApiResponse resp = reply.get();
            auto et = gpr_now(GPR_CLOCK_REALTIME);
            auto t = gpr_time_to_millis(gpr_time_sub(et, st));
            gpr_log(GPR_DEBUG, "LargeRequestTime(ms): %-5d %s %p Size: %d", t, method.c_str(), &client, resp.status());
        }
        catch (std::exception& e)
        {
            cerr << "LargeRequest failed: " << e.what() << endl;
        }

        sleep(3);
    }
}

void SendSmallRequest(std::shared_ptr<Channel> address, int sec)
{
    sleep(sec);
    while (true)
    {
        HttpApiClient client(address);
        client.SetTimeoutMs(20);
        //gpr_log(GPR_INFO, "rpc start: %p", &client);
        try
        {
            auto st = gpr_now(GPR_CLOCK_REALTIME);
            std::future<HttpApiResponse> reply = client.Request("GET", "/echo", "hi");
            HttpApiResponse resp = reply.get();
            map<string, string> headers;
            for (int i=0; i < resp.headers_size(); i++)
            {
                auto& h = resp.headers().Get(i);
                headers[h.name()] = h.value();
            }
            auto et = gpr_now(GPR_CLOCK_REALTIME);
            auto t = gpr_time_to_millis(gpr_time_sub(et, st));
            if (t > 3)
            {
                gpr_log(GPR_INFO, "slow rpc(ms): %-5d %p st: %09d toStart: %09d onStarted: %09d serverStart: %s serverWrite: %s serverFinish: %s readDoneTime: %s doneTime: %s", t, &client, st.tv_nsec, client.mToStartTime, client.mOnStartTime, headers["serverStartTime"].c_str(), headers["serverWriteTime"].c_str(), headers["serverFinishTime"].c_str(), headers["readDoneTime"].c_str(), headers["doneTime"].c_str());
            }
            if (resp.status() != 200 || resp.body() != "hi")
            {
                cerr << "Bad response! status: " << resp.status() << "body: " << resp.body() << endl;
                exit(1);
            }
            //cout << "Response: " << resp.status() << ": " << resp.body() << endl;
        }
        catch (const std::exception& e)
        {
            cerr << "SmallRequest failed: " << e.what() << endl;
        }
        usleep(10 * 1000);
        //++qps;
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
            args.SetInt("grpc.http2.lookahead_bytes", 10000000);
            args.SetInt("grpc.http2.write_buffer_size", 10000000);
            auto channel = grpc::CreateCustomChannel(address, grpc::InsecureChannelCredentials(), args);
            //auto channel = server.InProcessChannel(args);

            std::thread t(SendLargeRequest, channel, "GET", 3);
            std::thread tt(SendLargeRequest, channel, "PUT", 2);
            std::thread ttt(SendLargeRequest, channel, "GET", 4);

            std::thread t1(SendSmallRequest, channel, 1);
            std::thread t2(SendSmallRequest, channel, 1);
            std::thread t3(SendSmallRequest, channel, 2);

            //uint64_t last_qps = 0;
            //while (true)
            //{
            //    uint64_t new_qps = qps.load();
            //    cout << "qps: " << new_qps - last_qps << endl;
            //    last_qps = new_qps;
            //    sleep(1);
            //}

            t1.join();
        }
    }
    catch (std::exception& e)
    {
        cerr << e.what() << endl;
    }
}
