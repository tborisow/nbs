#include "tablet.h"

#include <cloud/filestore/libs/storage/testlib/tablet_client.h>
#include <cloud/filestore/libs/storage/testlib/test_env.h>

#include <contrib/ydb/core/base/counters.h>

#include <library/cpp/testing/unittest/registar.h>

namespace NCloud::NFileStore::NStorage {

using namespace NActors;
using namespace NKikimr;

namespace {

////////////////////////////////////////////////////////////////////////////////

struct TTxStats
{
    i64 ROCacheHitCount = 0;
    i64 ROCacheMissCount = 0;
    i64 RWCount = 0;
};

TTxStats GetTxStats(TTestEnv& env, TIndexTabletClient& tablet)
{
    TTxStats stats;
    TTestRegistryVisitor visitor;

    tablet.SendRequest(tablet.CreateUpdateCounters());
    env.GetRuntime().DispatchEvents({}, TDuration::Seconds(1));
    env.GetRegistry()->Visit(TInstant::Zero(), visitor);

    visitor.ValidateExpectedCountersWithPredicate({
        {{{"filesystem", "test"},
          {"sensor", "InMemoryIndexStateROCacheHitCount"}},
         [&stats](i64 value)
         {
             stats.ROCacheHitCount = value;
             return true;
         }},
        {{{"filesystem", "test"},
          {"sensor", "InMemoryIndexStateROCacheMissCount"}},
         [&stats](i64 value)
         {
             stats.ROCacheMissCount = value;
             return true;
         }},
        {{{"filesystem", "test"}, {"sensor", "InMemoryIndexStateRWCount"}},
         [&stats](i64 value)
         {
             stats.RWCount = value;
             return true;
         }},
    });
    return stats;
}

}   // namespace

////////////////////////////////////////////////////////////////////////////////

Y_UNIT_TEST_SUITE(TIndexTabletTest_NodesCache)
{
    Y_UNIT_TEST(ShouldUpdateAndEvictCacheUponCreateNode)
    {
        NProto::TStorageConfig storageConfig;
        storageConfig.SetInMemoryIndexCacheEnabled(true);
        storageConfig.SetInMemoryIndexCacheNodesCapacity(2);
        storageConfig.SetInMemoryIndexCacheNodeRefsCapacity(1);
        TTestEnv env({}, storageConfig);
        env.CreateSubDomain("nfs");

        ui32 nodeIdx = env.CreateNode("nfs");
        ui64 tabletId = env.BootIndexTablet(nodeIdx);

        TIndexTabletClient tablet(env.GetRuntime(), nodeIdx, tabletId);
        tablet.InitSession("client", "session");

        auto statsBefore = GetTxStats(env, tablet);

        auto id = tablet.CreateNode(TCreateNodeArgs::File(RootNodeId, "test"))
                      ->Record.GetNode()
                      .GetId();
        // Create node should have populated the cache, thus the next
        // GetNodeAttr should not trigger a transaction.
        {
            auto node =
                tablet.GetNodeAttr(RootNodeId, "test")->Record.GetNode();
            UNIT_ASSERT_VALUES_EQUAL(id, node.GetId());
            UNIT_ASSERT(node.GetType() == NProto::E_REGULAR_NODE);
        }

        // The following GetNodeAttr should not produce a transaction either, as
        // the requested node is already in the cache.
        {
            auto node = tablet.GetNodeAttr(RootNodeId, "")->Record.GetNode();
            UNIT_ASSERT_VALUES_EQUAL(RootNodeId, node.GetId());
            UNIT_ASSERT(node.GetType() == NProto::E_DIRECTORY_NODE);
        }

        // Now the Nodes table has two entries: Root and test.
        // The NodeRefs table has one entry: Root -> test.
        //
        // Creating another node should evict the first one from the cache
        tablet.CreateNode(TCreateNodeArgs::File(RootNodeId, "test2"));
        UNIT_ASSERT_VALUES_EQUAL(
            id,
            tablet.GetNodeAttr(RootNodeId, "test")->Record.GetNode().GetId());
        // ListNodes can not be performed using in-memory cache
        tablet.ListNodes(RootNodeId);

        // Two out of three GetNodeAttr calls should have been performed using
        // the cache. ListNodes is a cache miss also.
        auto statsAfter = GetTxStats(env, tablet);
        UNIT_ASSERT_VALUES_EQUAL(
            2,
            statsAfter.ROCacheHitCount - statsBefore.ROCacheHitCount);
        UNIT_ASSERT_VALUES_EQUAL(
            1,
            statsAfter.ROCacheMissCount - statsBefore.ROCacheMissCount);
        UNIT_ASSERT_VALUES_EQUAL(2, statsAfter.RWCount - statsBefore.RWCount);
    }

    // Note: this test does not check the cache eviction policy, as cache size
    // changes are not expected upon node rename
    Y_UNIT_TEST(ShouldUpdateCacheUponRenameNode)
    {
        NProto::TStorageConfig storageConfig;
        storageConfig.SetInMemoryIndexCacheEnabled(true);
        storageConfig.SetInMemoryIndexCacheNodesCapacity(3);
        storageConfig.SetInMemoryIndexCacheNodeRefsCapacity(2);
        TTestEnv env({}, storageConfig);
        env.CreateSubDomain("nfs");

        ui32 nodeIdx = env.CreateNode("nfs");
        ui64 tabletId = env.BootIndexTablet(nodeIdx);

        TIndexTabletClient tablet(env.GetRuntime(), nodeIdx, tabletId);
        tablet.InitSession("client", "session");

        auto statsBefore = GetTxStats(env, tablet);

        // RW transaction, populates the cache
        tablet.CreateNode(TCreateNodeArgs::File(RootNodeId, "test"));
        // RO transaction, cache hit
        auto ctime =
            tablet.GetNodeAttr(RootNodeId, "")->Record.GetNode().GetCTime();

        // Upon rename node the wall clock time is used to update the ctime, so
        // until this is replaced with ctx.Now, we need to perform sleep, not
        // tablet.AdvanceTime
        Sleep(TDuration::Seconds(2));

        // RW transaction, Nodes table has 2 entries: Root and test2, NodeRefs
        // has 1 entry: Root -> test2
        tablet.RenameNode(RootNodeId, "test", RootNodeId, "test2");

        // RO transaction, cache hit
        auto newCtime =
            tablet.GetNodeAttr(RootNodeId, "")->Record.GetNode().GetCTime();
        UNIT_ASSERT_GT(newCtime, ctime);

        // GetNodeAttr of a non-existing node can not be performed using the
        // cache, this is a RO transaction, cache miss
        tablet.AssertGetNodeAttrFailed(RootNodeId, "test");

        // Two out of three should have been performed using the cache
        auto statsAfter = GetTxStats(env, tablet);
        UNIT_ASSERT_VALUES_EQUAL(
            2,
            statsAfter.ROCacheHitCount - statsBefore.ROCacheHitCount);
        UNIT_ASSERT_VALUES_EQUAL(
            1,
            statsAfter.ROCacheMissCount - statsBefore.ROCacheMissCount);
        UNIT_ASSERT_VALUES_EQUAL(2, statsAfter.RWCount - statsBefore.RWCount);
    }

    Y_UNIT_TEST(ShouldUpdateCacheUponUnlinkNode)
    {
        NProto::TStorageConfig storageConfig;
        storageConfig.SetInMemoryIndexCacheEnabled(true);
        storageConfig.SetInMemoryIndexCacheNodesCapacity(3);
        storageConfig.SetInMemoryIndexCacheNodeRefsCapacity(3);
        TTestEnv env({}, storageConfig);
        env.CreateSubDomain("nfs");

        ui32 nodeIdx = env.CreateNode("nfs");
        ui64 tabletId = env.BootIndexTablet(nodeIdx);

        TIndexTabletClient tablet(env.GetRuntime(), nodeIdx, tabletId);
        tablet.InitSession("client", "session");

        auto statsBefore = GetTxStats(env, tablet);

        // RW transaction, populates the cache
        auto id1 =
            CreateNode(tablet, TCreateNodeArgs::File(RootNodeId, "test1"));
        auto id2 =
            CreateNode(tablet, TCreateNodeArgs::File(RootNodeId, "test2"));

        // Both nodes are in the cache. RO transaction, cache hit
        tablet.GetNodeAttr(id1, "");
        tablet.GetNodeAttr(id2, "");
        tablet.GetNodeAttr(RootNodeId, "test1");
        tablet.GetNodeAttr(RootNodeId, "test2");

        // RW transactions, cache should be updated and nodes should be removed
        tablet.UnlinkNode(RootNodeId, "test1", false);
        tablet.UnlinkNode(RootNodeId, "test2", false);

        // All four requests should have been performed without the cache
        tablet.AssertGetNodeAttrFailed(id1, "");
        tablet.AssertGetNodeAttrFailed(id2, "");
        tablet.AssertGetNodeAttrFailed(RootNodeId, "test1");
        tablet.AssertGetNodeAttrFailed(RootNodeId, "test2");

        // Four out of eight GetNodeAttr calls should have been performed using
        // the cache. Other four are cache misses and should have failed
        auto statsAfter = GetTxStats(env, tablet);
        UNIT_ASSERT_VALUES_EQUAL(
            4,
            statsAfter.ROCacheHitCount - statsBefore.ROCacheHitCount);
        UNIT_ASSERT_VALUES_EQUAL(
            4,
            statsAfter.ROCacheMissCount - statsBefore.ROCacheMissCount);
        UNIT_ASSERT_VALUES_EQUAL(4, statsAfter.RWCount - statsBefore.RWCount);
    }

    Y_UNIT_TEST(ShouldUpdateCacheUponDestroySession)
    {
        NProto::TStorageConfig storageConfig;
        storageConfig.SetInMemoryIndexCacheEnabled(true);
        storageConfig.SetInMemoryIndexCacheNodesCapacity(2);
        storageConfig.SetInMemoryIndexCacheNodeRefsCapacity(2);
        TTestEnv env({}, storageConfig);
        env.CreateSubDomain("nfs");

        ui32 nodeIdx = env.CreateNode("nfs");
        ui64 tabletId = env.BootIndexTablet(nodeIdx);

        TIndexTabletClient tablet(env.GetRuntime(), nodeIdx, tabletId);
        tablet.InitSession("client", "session");

        auto statsBefore = GetTxStats(env, tablet);

        auto id = tablet.CreateNode(TCreateNodeArgs::File(RootNodeId, "test"))
                      ->Record.GetNode()
                      .GetId();
        auto handle = tablet.CreateHandle(id, TCreateHandleArgs::RDWR);

        // Cache hit
        tablet.GetNodeAttr(id, "")->Record.GetNode();

        tablet.UnlinkNode(RootNodeId, "test", false);
        // Should work as there is an existing handle
        tablet.GetNodeAttr(id, "");

        // Upon session destruction the node should be removed from the cache as
        // well as the localDB
        tablet.DestroySession();

        tablet.InitSession("client", "session2");

        UNIT_ASSERT_VALUES_EQUAL(
            E_FS_NOENT,
            tablet.AssertGetNodeAttrFailed(id, "")->GetError().GetCode());

        auto statsAfter = GetTxStats(env, tablet);
        // First two GetNodeAttr calls should have been performed with the cache
        UNIT_ASSERT_VALUES_EQUAL(
            2,
            statsAfter.ROCacheHitCount - statsBefore.ROCacheHitCount);
        // Last GetNodeAttr call should have been a cache miss as it is not
        // supposed to be present in the cache
        UNIT_ASSERT_VALUES_EQUAL(
            1,
            statsAfter.ROCacheMissCount - statsBefore.ROCacheMissCount);
        // CreateNode, CreateHandle, UnlinkNode, DestroySession and InitSession
        // are RW txs
        UNIT_ASSERT_VALUES_EQUAL(5, statsAfter.RWCount - statsBefore.RWCount);
    }

    Y_UNIT_TEST(ShouldUpdateCacheUponSetNodeAttr)
    {
        NProto::TStorageConfig storageConfig;
        storageConfig.SetInMemoryIndexCacheEnabled(true);
        storageConfig.SetInMemoryIndexCacheNodesCapacity(2);
        storageConfig.SetInMemoryIndexCacheNodeRefsCapacity(1);
        TTestEnv env({}, storageConfig);
        env.CreateSubDomain("nfs");

        ui32 nodeIdx = env.CreateNode("nfs");
        ui64 tabletId = env.BootIndexTablet(nodeIdx);

        TIndexTabletClient tablet(env.GetRuntime(), nodeIdx, tabletId);
        tablet.InitSession("client", "session");

        auto statsBefore = GetTxStats(env, tablet);

        // RW transaction, populates the cache
        auto id = tablet.CreateNode(TCreateNodeArgs::File(RootNodeId, "test"))
                      ->Record.GetNode()
                      .GetId();

        // RW transaction, updates cache
        tablet.SetNodeAttr(TSetNodeAttrArgs(id).SetSize(77));

        // RO transaction, cache hit
        UNIT_ASSERT_VALUES_EQUAL(
            77,
            tablet.GetNodeAttr(id, "")->Record.GetNode().GetSize());

        auto statsAfter = GetTxStats(env, tablet);
        // The only one GetNodeAttr call should have been performed with the
        // cache
        UNIT_ASSERT_VALUES_EQUAL(
            1,
            statsAfter.ROCacheHitCount - statsBefore.ROCacheHitCount);
        // There was no cache misses
        UNIT_ASSERT_VALUES_EQUAL(
            0,
            statsAfter.ROCacheMissCount - statsBefore.ROCacheMissCount);
        // CreateNode and SetNodeAttr are RW txs
        UNIT_ASSERT_VALUES_EQUAL(2, statsAfter.RWCount - statsBefore.RWCount);
    }

    Y_UNIT_TEST(ShouldUpdateCacheUponSetNodeXAttr)
    {
        NProto::TStorageConfig storageConfig;
        storageConfig.SetInMemoryIndexCacheEnabled(true);
        storageConfig.SetInMemoryIndexCacheNodesCapacity(2);
        storageConfig.SetInMemoryIndexCacheNodeRefsCapacity(1);
        storageConfig.SetInMemoryIndexCacheNodeAttrsCapacity(2);
        TTestEnv env({}, storageConfig);
        env.CreateSubDomain("nfs");

        ui32 nodeIdx = env.CreateNode("nfs");
        ui64 tabletId = env.BootIndexTablet(nodeIdx);

        TIndexTabletClient tablet(env.GetRuntime(), nodeIdx, tabletId);
        tablet.InitSession("client", "session");

        auto statsBefore = GetTxStats(env, tablet);

        // RW transaction, populates the cache
        auto id = tablet.CreateNode(TCreateNodeArgs::File(RootNodeId, "test"))
                      ->Record.GetNode()
                      .GetId();

        // RW transaction, updates cache
        tablet.SetNodeXAttr(id, "user.test", "value");

        // RO transaction, cache hit
        UNIT_ASSERT_VALUES_EQUAL(
            "value",
            tablet.GetNodeXAttr(id, "user.test")->Record.GetValue());

        // RW transaction, updates cache
        tablet.SetNodeXAttr(id, "user.test", "value2");
        // RW transaction, updates cache
        tablet.SetNodeXAttr(id, "user.test2", "value");

        // RO transactions, cache hit
        UNIT_ASSERT_VALUES_EQUAL(
            "value2",
            tablet.GetNodeXAttr(id, "user.test")->Record.GetValue());
        UNIT_ASSERT_VALUES_EQUAL(
            "value",
            tablet.GetNodeXAttr(id, "user.test2")->Record.GetValue());

        // RW transaction, evicts the cache
        tablet.SetNodeXAttr(id, "user.test3", "value");

        // RO transaction, cache miss
        UNIT_ASSERT_VALUES_EQUAL(
            "value",
            tablet.GetNodeXAttr(id, "user.test2")->Record.GetValue());

        auto statsAfter = GetTxStats(env, tablet);

        UNIT_ASSERT_VALUES_EQUAL(
            3,
            statsAfter.ROCacheHitCount - statsBefore.ROCacheHitCount);
        UNIT_ASSERT_VALUES_EQUAL(
            1,
            statsAfter.ROCacheMissCount - statsBefore.ROCacheMissCount);
        UNIT_ASSERT_VALUES_EQUAL(5, statsAfter.RWCount - statsBefore.RWCount);
    }

    Y_UNIT_TEST(ShouldUpdateCacheUponRemoveNodeXAttr)
    {
        NProto::TStorageConfig storageConfig;
        storageConfig.SetInMemoryIndexCacheEnabled(true);
        storageConfig.SetInMemoryIndexCacheNodesCapacity(2);
        storageConfig.SetInMemoryIndexCacheNodeRefsCapacity(1);
        storageConfig.SetInMemoryIndexCacheNodeAttrsCapacity(2);
        TTestEnv env({}, storageConfig);
        env.CreateSubDomain("nfs");

        ui32 nodeIdx = env.CreateNode("nfs");
        ui64 tabletId = env.BootIndexTablet(nodeIdx);

        TIndexTabletClient tablet(env.GetRuntime(), nodeIdx, tabletId);
        tablet.InitSession("client", "session");

        auto statsBefore = GetTxStats(env, tablet);

        // RW transaction, populates the cache
        auto id = tablet.CreateNode(TCreateNodeArgs::File(RootNodeId, "test"))
                      ->Record.GetNode()
                      .GetId();

        // RW transaction, updates cache
        tablet.SetNodeXAttr(id, "user.test", "value");

        // RO transaction, cache hit
        UNIT_ASSERT_VALUES_EQUAL(
            "value",
            tablet.GetNodeXAttr(id, "user.test")->Record.GetValue());

        // RW transaction, updates cache
        tablet.RemoveNodeXAttr(id, "user.test");

        // RO transaction, cache miss
        UNIT_ASSERT_VALUES_EQUAL(
            MAKE_FILESTORE_ERROR(NProto::E_FS_NOXATTR),
            tablet.AssertGetNodeXAttrFailed(id, "user.test")
                ->GetError()
                .GetCode());

        auto statsAfter = GetTxStats(env, tablet);

        UNIT_ASSERT_VALUES_EQUAL(
            1,
            statsAfter.ROCacheHitCount - statsBefore.ROCacheHitCount);
        UNIT_ASSERT_VALUES_EQUAL(
            1,
            statsAfter.ROCacheMissCount - statsBefore.ROCacheMissCount);
        UNIT_ASSERT_VALUES_EQUAL(3, statsAfter.RWCount - statsBefore.RWCount);
    }

    Y_UNIT_TEST(ShouldUpdateCacheUponWriteAndCreateHandle)
    {
        NProto::TStorageConfig storageConfig;
        storageConfig.SetInMemoryIndexCacheEnabled(true);
        storageConfig.SetInMemoryIndexCacheNodesCapacity(2);
        storageConfig.SetInMemoryIndexCacheNodeRefsCapacity(1);
        TTestEnv env({}, storageConfig);
        env.CreateSubDomain("nfs");

        ui32 nodeIdx = env.CreateNode("nfs");
        ui64 tabletId = env.BootIndexTablet(nodeIdx);

        TIndexTabletClient tablet(env.GetRuntime(), nodeIdx, tabletId);
        tablet.InitSession("client", "session");

        auto statsBefore = GetTxStats(env, tablet);

        // RW transaction, populates the cache
        auto id = tablet.CreateNode(TCreateNodeArgs::File(RootNodeId, "test"))
                      ->Record.GetNode()
                      .GetId();
        // RW transaction
        auto handle = tablet.CreateHandle(id, TCreateHandleArgs::RDWR)
                          ->Record.GetHandle();
        // RW transaction, updates file size
        tablet.WriteData(handle, 0, 99, '0');

        // RO transaction, cache hit
        UNIT_ASSERT_VALUES_EQUAL(
            99,
            tablet.GetNodeAttr(id, "")->Record.GetNode().GetSize());

        // RW transaction, updates cache
        tablet.CreateHandle(
            id,
            "",
            TCreateHandleArgs::CREATE | TCreateHandleArgs::TRUNC);

        // RO transaction, cache hit
        UNIT_ASSERT_VALUES_EQUAL(
            0,
            tablet.GetNodeAttr(id, "")->Record.GetNode().GetSize());

        auto statsAfter = GetTxStats(env, tablet);

        UNIT_ASSERT_VALUES_EQUAL(
            2,
            statsAfter.ROCacheHitCount - statsBefore.ROCacheHitCount);
        UNIT_ASSERT_VALUES_EQUAL(
            0,
            statsAfter.ROCacheMissCount - statsBefore.ROCacheMissCount);
        UNIT_ASSERT_VALUES_EQUAL(4, statsAfter.RWCount - statsBefore.RWCount);
    }

    Y_UNIT_TEST(ShouldUpdateCacheUponDestroyHandle)
    {
        NProto::TStorageConfig storageConfig;
        storageConfig.SetInMemoryIndexCacheEnabled(true);
        storageConfig.SetInMemoryIndexCacheNodesCapacity(2);
        storageConfig.SetInMemoryIndexCacheNodeRefsCapacity(1);
        TTestEnv env({}, storageConfig);
        env.CreateSubDomain("nfs");

        ui32 nodeIdx = env.CreateNode("nfs");
        ui64 tabletId = env.BootIndexTablet(nodeIdx);

        TIndexTabletClient tablet(env.GetRuntime(), nodeIdx, tabletId);
        tablet.InitSession("client", "session");

        auto statsBefore = GetTxStats(env, tablet);

        // RW transaction, populates the cache
        auto id = tablet.CreateNode(TCreateNodeArgs::File(RootNodeId, "test"))
                      ->Record.GetNode()
                      .GetId();
        // RW transaction, updates cache
        auto handle = tablet.CreateHandle(id, TCreateHandleArgs::RDWR)
                          ->Record.GetHandle();

        // RW transaction, updates cache
        tablet.UnlinkNode(RootNodeId, "test", false);
        // RO transaction, cache hit
        tablet.GetNodeAttr(id, "");
        // RW transaction, updates cache
        tablet.DestroyHandle(handle);
        // RO transaction, cache miss
        tablet.AssertGetNodeAttrFailed(id, "");

        auto statsAfter = GetTxStats(env, tablet);

        UNIT_ASSERT_VALUES_EQUAL(
            1,
            statsAfter.ROCacheHitCount - statsBefore.ROCacheHitCount);
        UNIT_ASSERT_VALUES_EQUAL(
            1,
            statsAfter.ROCacheMissCount - statsBefore.ROCacheMissCount);
        UNIT_ASSERT_VALUES_EQUAL(4, statsAfter.RWCount - statsBefore.RWCount);
    }

    Y_UNIT_TEST(ShouldUpdateCacheUponAddBlob)
    {
        NProto::TStorageConfig storageConfig;
        storageConfig.SetInMemoryIndexCacheEnabled(true);
        storageConfig.SetInMemoryIndexCacheNodesCapacity(2);
        storageConfig.SetInMemoryIndexCacheNodeRefsCapacity(1);
        TTestEnv env({}, storageConfig);
        env.CreateSubDomain("nfs");

        ui32 nodeIdx = env.CreateNode("nfs");
        ui64 tabletId = env.BootIndexTablet(nodeIdx);

        TIndexTabletClient tablet(env.GetRuntime(), nodeIdx, tabletId);
        tablet.InitSession("client", "session");

        auto statsBefore = GetTxStats(env, tablet);

        // RW transaction, populates the cache
        auto id = tablet.CreateNode(TCreateNodeArgs::File(RootNodeId, "test"))
                      ->Record.GetNode()
                      .GetId();
        // RW transaction, updates cache
        auto handle = tablet.CreateHandle(id, TCreateHandleArgs::RDWR)
                          ->Record.GetHandle();

        // explicitly write data to blob storage
        const auto dataSize = DefaultBlockSize * BlockGroupSize;
        NKikimr::TLogoBlobID blobId;
        ui64 commitId = 0;
        {
            auto gbi = tablet
                           .GenerateBlobIds(
                               id,
                               handle,
                               /* offset */ 0,
                               /* length */ dataSize)
                           ->Record;
            UNIT_ASSERT_VALUES_EQUAL(1, gbi.BlobsSize());
            commitId = gbi.GetCommitId();

            blobId = LogoBlobIDFromLogoBlobID(gbi.GetBlobs(0).GetBlobId());
            auto evPut = std::make_unique<TEvBlobStorage::TEvPut>(
                blobId,
                TString(dataSize, 'x'),
                TInstant::Max(),
                NKikimrBlobStorage::UserData);
            NKikimr::TActorId proxy =
                MakeBlobStorageProxyID(gbi.GetBlobs(0).GetBSGroupId());
            auto evPutSender =
                env.GetRuntime().AllocateEdgeActor(proxy.NodeId());
            env.GetRuntime().Send(CreateEventForBSProxy(
                evPutSender,
                proxy,
                evPut.release(),
                blobId.Cookie()));
        }

        // RW transaction, updates cache, subsequently calls AddData
        tablet.AddData(
            id,
            handle,
            /* offset */ 0,
            /* length */ dataSize,
            TVector<NKikimr::TLogoBlobID>{blobId},
            commitId,
            /* unaligned parts */ TVector<NProtoPrivate::TFreshDataRange>{});

        // RO transaction, cache hit
        UNIT_ASSERT_VALUES_EQUAL(
            dataSize,
            tablet.GetNodeAttr(id, "")->Record.GetNode().GetSize());

        auto statsAfter = GetTxStats(env, tablet);

        UNIT_ASSERT_VALUES_EQUAL(
            1,
            statsAfter.ROCacheHitCount - statsBefore.ROCacheHitCount);
        UNIT_ASSERT_VALUES_EQUAL(
            0,
            statsAfter.ROCacheMissCount - statsBefore.ROCacheMissCount);
        UNIT_ASSERT_VALUES_EQUAL(4, statsAfter.RWCount - statsBefore.RWCount);
    }

    Y_UNIT_TEST(ShouldUpdateCacheUponAllocateData)
    {
        NProto::TStorageConfig storageConfig;
        storageConfig.SetInMemoryIndexCacheEnabled(true);
        storageConfig.SetInMemoryIndexCacheNodesCapacity(2);
        storageConfig.SetInMemoryIndexCacheNodeRefsCapacity(1);
        TTestEnv env({}, storageConfig);
        env.CreateSubDomain("nfs");

        ui32 nodeIdx = env.CreateNode("nfs");
        ui64 tabletId = env.BootIndexTablet(nodeIdx);

        TIndexTabletClient tablet(env.GetRuntime(), nodeIdx, tabletId);
        tablet.InitSession("client", "session");

        auto statsBefore = GetTxStats(env, tablet);

        // RW transaction, populates the cache
        auto id = tablet.CreateNode(TCreateNodeArgs::File(RootNodeId, "test"))
                      ->Record.GetNode()
                      .GetId();
        // RW transaction, updates cache
        auto handle = tablet.CreateHandle(id, TCreateHandleArgs::RDWR)
                          ->Record.GetHandle();

        auto newSize = DefaultBlockSize * BlockGroupSize;
        // RW transaction, updates cache
        tablet.AllocateData(
            handle,
            0,
            newSize,
            ProtoFlag(NProto::TAllocateDataRequest::F_ZERO_RANGE));

        // RO transaction, cache hit
        UNIT_ASSERT_VALUES_EQUAL(
            newSize,
            tablet.GetNodeAttr(id, "")->Record.GetNode().GetSize());

        auto statsAfter = GetTxStats(env, tablet);

        UNIT_ASSERT_VALUES_EQUAL(
            1,
            statsAfter.ROCacheHitCount - statsBefore.ROCacheHitCount);
        UNIT_ASSERT_VALUES_EQUAL(
            0,
            statsAfter.ROCacheMissCount - statsBefore.ROCacheMissCount);
        UNIT_ASSERT_VALUES_EQUAL(3, statsAfter.RWCount - statsBefore.RWCount);
    }
}

}   // namespace NCloud::NFileStore::NStorage
