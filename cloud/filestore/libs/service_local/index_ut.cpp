#include "index.h"

#include <cloud/filestore/libs/service/filestore.h>

#include <library/cpp/testing/unittest/registar.h>

#include <util/folder/tempdir.h>

namespace NCloud::NFileStore {

namespace {

////////////////////////////////////////////////////////////////////////////////

struct TEnvironment
    : public NUnitTest::TBaseFixture
{
    ILoggingServicePtr Logging =
        CreateLoggingService("console", {TLOG_RESOURCES});
    TLog Log;

    const TTempDir TempDir;
    const TFsPath RootPath = TempDir.Path() / "root";
    const TFsPath StatePath = TempDir.Path() / "state";

    TEnvironment() : Log(Logging->CreateLog("INDEX_TEST"))
    {
        RootPath.MkDir();
        StatePath.MkDir();
    }

protected:
    void CreateNestedDir(
        ui32 pathLen,
        TMap<TString, ui64>& nodeMap)
    {
        TLocalIndex index(RootPath, StatePath, pathLen, Log);

        auto node = index.LookupNode(RootNodeId);
        UNIT_ASSERT_C(node, "Failed to lookup RootNode");

        auto path = RootPath;
        for (ui32 i = 0; i < pathLen; i++) {
            TString name = ToString(i);
            path = path / name;
            path.MkDir();

            auto childNode = TIndexNode::Create(*node, name);
            UNIT_ASSERT_C(childNode, "Failed to create node: " << name);

            STORAGE_DEBUG(
                "NodeId=" << childNode->GetNodeId() << " ,Name=" << name);

            auto inserted =
                index.TryInsertNode(childNode, node->GetNodeId(), name);
            UNIT_ASSERT_C(inserted, "Failed to insert node: " << name);

            nodeMap.emplace(name, childNode->GetNodeId());
            UNIT_ASSERT_LT_C(
                node->GetNodeId(),
                childNode->GetNodeId(),
                "node id=" <<  node->GetNodeId() <<
                " , child node id=" << childNode->GetNodeId());
            node = childNode;
        }
    }

    void CreateReversedNodeIdNestedDir(
        ui32 pathLen,
        TMap<TString, ui64>& nodeMap)
    {
        TLocalIndex index(RootPath, StatePath, pathLen, Log);

        auto node = index.LookupNode(RootNodeId);
        UNIT_ASSERT_C(node, "Failed to lookup RootNode");

        auto path = RootPath;
        for (ui32 i = 0; i < pathLen; i++) {
            TString name = ToString(pathLen - 1 - i);
            path = RootPath / name;
            path.MkDir();

            if (i > 0) {
                TString prevName = ToString(pathLen - i);
                auto prevPath = RootPath / prevName;
                prevPath.RenameTo(RootPath / name / prevName);
            }
        }

        for (ui32 i = 0; i < pathLen; i++) {
            TString name = ToString(i);

            auto childNode = TIndexNode::Create(*node, name);
            UNIT_ASSERT_C(childNode, "Failed to create node: " << name);

            STORAGE_DEBUG(
                "NodeId=" << childNode->GetNodeId() << " ,Name=" << name);

            auto inserted =
                index.TryInsertNode(childNode, node->GetNodeId(), name);
            UNIT_ASSERT_C(inserted, "Failed to insert node: " << name);

            nodeMap.emplace(name, childNode->GetNodeId());
            if (i > 0) {
                UNIT_ASSERT_GT_C(
                    node->GetNodeId(),
                    childNode->GetNodeId(),
                    "node id=" <<  node->GetNodeId() <<
                    " , child node id=" << childNode->GetNodeId());
            }
            node = childNode;
        }
    }

    void CheckNestedDir(ui32 pathLen, const TMap<TString, ui64>& nodeMap)
    {
        TLocalIndex index(RootPath, StatePath, pathLen, Log);
        auto node = index.LookupNode(RootNodeId);
        UNIT_ASSERT_C(node, "Failed to lookup root node");

        for (ui32 i = 0; i < pathLen; i++) {
            TString name = ToString(i);

            auto nodes = node->List();
            UNIT_ASSERT_VALUES_EQUAL(nodes.size(), 1);

            auto& [nodeName, nodeStat] = nodes[0];
            UNIT_ASSERT_VALUES_EQUAL(name, nodeName);

            auto it = nodeMap.find(nodeName);
            UNIT_ASSERT_C(it != nodeMap.end(), "node not found: " << nodeName);

            auto nodeId = it->second;
            STORAGE_DEBUG("Found node: " << nodeName << ", NodeId=" << nodeId);

            node = index.LookupNode(nodeId);
            UNIT_ASSERT_C(node,
                "Failed to lookup  node id: " << nodeId <<
                ", node: " << nodeName);
        }
    }

};

struct TNodeTableHeader
{
};

struct TNodeTableRecord
{
    ui64 NodeId = 0;
    ui64 ParentNodeId = 0;
    char Name[NAME_MAX + 1];
};

using TNodeTable = TPersistentTable<TNodeTableHeader, TNodeTableRecord>;

}   // namespace

////////////////////////////////////////////////////////////////////////////////

Y_UNIT_TEST_SUITE(TLocalIndex)
{
    Y_UNIT_TEST_F(ShouldRecoverNestedDir, TEnvironment)
    {
        ui32 pathLen = 10;
        TMap<TString, ui64> nodeMap;

        CreateNestedDir(pathLen, nodeMap);
        CheckNestedDir(pathLen, nodeMap);
    }

    Y_UNIT_TEST_F(ShouldRecoverNestedDirWithReversedInodeOrder, TEnvironment)
    {
        ui32 pathLen = 10;
        TMap<TString, ui64> nodeMap;

        CreateReversedNodeIdNestedDir(pathLen, nodeMap);
        CheckNestedDir(pathLen, nodeMap);

    }
};

}   // namespace NCloud::NFileStore
