#include <cloud/storage/core/libs/diagnostics/logging.h>

#include <contrib/ydb/public/sdk/cpp/client/iam/common/iam.h>
#include <contrib/ydb/public/sdk/cpp/client/ydb_persqueue_core/ut/ut_utils/test_server.h>
#include <contrib/ydb/public/sdk/cpp/client/ydb_topic/topic.h>
#include <contrib/ydb/public/sdk/cpp/client/ydb_types/status_codes.h>

#include <library/cpp/testing/unittest/registar.h>

#include <util/generic/string.h>

using namespace NThreading;

namespace {

////////////////////////////////////////////////////////////////////////////////

const TString TestConsumer = "test-consumer";
const TString TestTopic = "test-topic";
const TString TestSource = "test-source";
const TString Database = "/Root";

////////////////////////////////////////////////////////////////////////////////

struct TFixture
    : public NUnitTest::TBaseFixture
{
    NCloud::ILoggingServicePtr Logging = NCloud::CreateLoggingService(
        "console",
        {.FiltrationLevel = TLOG_DEBUG, .UseLocalTimestamps = true});

    std::optional<NPersQueue::TTestServer> TestServer;
    std::optional<NYdb::TDriver> Driver;

    void SetUp(NUnitTest::TTestContext&) override
    {
        using namespace NYdb::NTopic;

        TestServer.emplace(CreateServerSettings());

        Driver.emplace(NYdb::TDriverConfig()
            .SetEndpoint("localhost:" + ToString(TestServer->GrpcPort))
            .SetDatabase(Database)
            .SetLog(Logging->CreateLog("BLOCKSTORE_LOGBROKER").ReleaseBackend())
            .SetDiscoveryMode(NYdb::EDiscoveryMode::Async)
            .SetDrainOnDtors(true));

        TTopicClient client{*Driver};

        auto settings = TCreateTopicSettings()
            .PartitioningSettings(1, 1);

        TConsumerSettings consumers(
            settings,
            TestConsumer);

        settings.AppendConsumers(consumers);

        auto status = client.CreateTopic(TestTopic, settings)
            .GetValueSync();

        UNIT_ASSERT_C(status.IsSuccess(), status);

        TestServer->WaitInit(TestTopic);
    }

    static NKikimr::Tests::TServerSettings CreateServerSettings()
    {
        using namespace NKikimrServices;
        using namespace NActors::NLog;

        auto settings = NKikimr::NPersQueueTests::PQSettings(0);
        settings.SetDomainName("Root");
        settings.SetNodeCount(1);
        settings.PQConfig.SetTopicsAreFirstClassCitizen(true);
        settings.PQConfig.SetRoot("/Root");
        settings.PQConfig.SetDatabase("/Root");

        settings.SetLoggerInitializer([] (NActors::TTestActorRuntime& runtime) {
            runtime.SetLogPriority(PQ_READ_PROXY, PRI_DEBUG);
            runtime.SetLogPriority(PQ_WRITE_PROXY, PRI_DEBUG);
            runtime.SetLogPriority(PQ_MIRRORER, PRI_DEBUG);
            runtime.SetLogPriority(PQ_METACACHE, PRI_DEBUG);
            runtime.SetLogPriority(PERSQUEUE, PRI_DEBUG);
            runtime.SetLogPriority(PERSQUEUE_CLUSTER_TRACKER, PRI_DEBUG);
        });

        return settings;
    }
};

////////////////////////////////////////////////////////////////////////////////

struct TWriteOp
{
    TString Message;
    ui64 SeqNo = 0;
    bool Written = false;
    bool Acknowledged = false;
    std::shared_ptr<NYdb::NTopic::IWriteSession> Session;
    TPromise<void> Promise;

    bool ProcessEvent(NYdb::NTopic::TWriteSessionEvent::TEvent& event)
    {
        using namespace NYdb::NTopic;

        return std::visit(
            TOverloaded{
                [&](TWriteSessionEvent::TReadyToAcceptEvent& e)
                {
                    if (!Written) {
                        Session->Write(
                            std::move(e.ContinuationToken),
                            Message,
                            SeqNo,
                            Now());

                        Written = true;
                    }

                    return false;
                },
                [&](TWriteSessionEvent::TAcksEvent& e)
                {
                    Y_ABORT_UNLESS(
                        e.Acks.size() == 1 && SeqNo == e.Acks[0].SeqNo &&
                            TWriteSessionEvent::TWriteAck::EES_WRITTEN ==
                                e.Acks[0].State,
                        "%s",
                        e.DebugString().c_str());

                    Acknowledged = true;
                    Session->Close(TDuration{});

                    return false;
                },
                [&](const TSessionClosedEvent& e)
                {
                    Y_ABORT_UNLESS(
                        Written && Acknowledged,
                        "%s",
                        e.DebugString().c_str());

                    return true;
                },
            },
            event);
    }

    bool ProcessEvents()
    {
        bool done = false;

        while (TVector events = Session->GetEvents()) {
            for (auto& event: events) {
                if (ProcessEvent(event)) {
                    done = true;
                }
            }

            if (done) {
                Promise.SetValue();
                break;
            }
        }

        return done;
    }
};

void WaitEvent(std::shared_ptr<TWriteOp> op)
{
    auto session = op->Session;
    session->WaitEvent().Subscribe(
        [op = std::move(op)](const auto&) mutable
        {
            if (!op->ProcessEvents()) {
                WaitEvent(std::move(op));
            }
        });
}

TFuture<void> Write(NYdb::TDriver& driver, TString message, ui64 seqNo)
{
    using namespace NYdb::NTopic;

    TTopicClient client{driver};

    auto session = client.CreateWriteSession(
        TWriteSessionSettings()
            .Path(TestTopic)
            .ProducerId(TestSource)
            .MessageGroupId(TestSource)
            .RetryPolicy(NYdb::NTopic::IRetryPolicy::GetNoRetryPolicy()));

    auto op = std::make_shared<TWriteOp>();
    op->Message = std::move(message);
    op->Session = session;
    op->SeqNo = seqNo;
    op->Promise = NewPromise();

    auto future = op->Promise.GetFuture();

    WaitEvent(std::move(op));

    return future;
}

}   // namespace

////////////////////////////////////////////////////////////////////////////////

Y_UNIT_TEST_SUITE(TTopicApiTest)
{
    Y_UNIT_TEST_F(ShouldWriteData, TFixture)
    {
        const auto timeout = TDuration::Seconds(15);

        {
            auto hello = Write(*Driver, "Hello", 42);
            UNIT_ASSERT(hello.Wait(timeout));
            hello.GetValue();
        }

        {
            auto world = Write(*Driver, "World", 100);
            UNIT_ASSERT(world.Wait(timeout));
            world.GetValue();
        }
    }
}
