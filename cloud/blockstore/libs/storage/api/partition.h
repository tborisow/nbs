#pragma once

#include "public.h"

#include <cloud/blockstore/libs/kikimr/components.h>
#include <cloud/blockstore/libs/kikimr/events.h>

namespace NCloud::NBlockStore::NStorage::NPartition {

////////////////////////////////////////////////////////////////////////////////

#define BLOCKSTORE_PARTITION_REQUESTS(xxx, ...)                                \
    xxx(WaitReady,                       __VA_ARGS__)                          \
    xxx(StatPartition,                   __VA_ARGS__)                          \
    xxx(Drain,                           __VA_ARGS__)                          \
    xxx(EnterIncompleteMirrorRWMode,     __VA_ARGS__)                          \
// BLOCKSTORE_PARTITION_REQUESTS

// requests forwarded from service to partition
#define BLOCKSTORE_PARTITION_REQUESTS_FWD_SERVICE(xxx, ...)                    \
    xxx(ReadBlocks,         __VA_ARGS__)                                       \
    xxx(WriteBlocks,        __VA_ARGS__)                                       \
    xxx(ZeroBlocks,         __VA_ARGS__)                                       \
    xxx(CreateCheckpoint,   __VA_ARGS__)                                       \
    xxx(DeleteCheckpoint,   __VA_ARGS__)                                       \
    xxx(GetChangedBlocks,   __VA_ARGS__)                                       \
    xxx(ReadBlocksLocal,    __VA_ARGS__)                                       \
    xxx(WriteBlocksLocal,   __VA_ARGS__)                                       \
// BLOCKSTORE_PARTITION_REQUESTS_FWD_SERVICE

// requests forwarded from volume to partion
#define BLOCKSTORE_PARTITION_REQUESTS_FWD_VOLUME(xxx, ...)                     \
    xxx(DescribeBlocks,           __VA_ARGS__)                                 \
    xxx(GetUsedBlocks,            __VA_ARGS__)                                 \
    xxx(GetPartitionInfo,         __VA_ARGS__)                                 \
    xxx(CompactRange,             __VA_ARGS__)                                 \
    xxx(GetCompactionStatus,      __VA_ARGS__)                                 \
    xxx(DeleteCheckpointData,     __VA_ARGS__)                                 \
    xxx(RebuildMetadata,          __VA_ARGS__)                                 \
    xxx(GetRebuildMetadataStatus, __VA_ARGS__)                                 \
    xxx(ScanDisk,                 __VA_ARGS__)                                 \
    xxx(GetScanDiskStatus,        __VA_ARGS__)                                 \
// BLOCKSTORE_PARTITION_REQUESTS_FWD_VOLUME

////////////////////////////////////////////////////////////////////////////////

struct TEvPartition
{
    //
    // WaitReady
    //

    struct TWaitReadyRequest
    {
    };

    struct TWaitReadyResponse
    {
    };

    //
    // StatPartition
    //

    struct TStatPartitionRequest
    {
    };

    struct TStatPartitionResponse
    {
        NProto::TStatVolumeResponse Record;
    };

    //
    // Drain
    //

    struct TDrainRequest
    {
    };

    struct TDrainResponse
    {
    };

    //
    // Drain
    //

    struct TEnterIncompleteMirrorRWModeRequest
    {
        // 0 - for main devices; 1,2 - for mirror replicas
        const ui32 ReplicaIndex;
        TEnterIncompleteMirrorRWModeRequest(ui32 replicaIndex)
            : ReplicaIndex(replicaIndex)
        {}
    };

    struct TEnterIncompleteMirrorRWModeResponse
    {
    };

    //
    // Garbage collector finish report
    //

    struct TGarbageCollectorCompleted
    {
        const ui64 TabletId;
        TGarbageCollectorCompleted(ui64 tabletId)
            : TabletId(tabletId)
        {}
    };

    //
    // Events declaration
    //

    enum EEvents
    {
        EvBegin = TBlockStoreEvents::PARTITION_START,

        EvWaitReadyRequest,
        EvWaitReadyResponse,

        EvStatPartitionRequest,
        EvStatPartitionResponse,

        EvBackpressureReport,

        EvDrainRequest,
        EvDrainResponse,

        // ?? EvEnterIncompleteMirrorIOModeRequest
        // ?? IO -> RW
        EvEnterIncompleteMirrorRWModeRequest,
        EvEnterIncompleteMirrorRWModeResponse,

        EvExitIncompleteIOModeRequest,
        EvExitIncompleteIOModeResponse,

        EvGarbageCollectorCompleted,

        EvEnd
    };

    static_assert(EvEnd < (int)TBlockStoreEvents::PARTITION_END,
        "EvEnd expected to be < TBlockStoreEvents::PARTITION_END");

    BLOCKSTORE_PARTITION_REQUESTS(BLOCKSTORE_DECLARE_EVENTS)

    using TEvBackpressureReport = TRequestEvent<
        TBackpressureReport,
        EvBackpressureReport
    >;

    using TEvGarbageCollectorCompleted = TRequestEvent<
        TGarbageCollectorCompleted,
        EvGarbageCollectorCompleted
    >;
};

}   // namespace NCloud::NBlockStore::NStorage::NPartition
