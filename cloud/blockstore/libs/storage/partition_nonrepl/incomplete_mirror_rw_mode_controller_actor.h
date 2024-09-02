#pragma once

#include "public.h"

#include "checksum_range.h"
#include "config.h"
#include "part_mirror_state.h"
#include "part_nonrepl_events_private.h"

#include <cloud/blockstore/libs/diagnostics/public.h>
#include <cloud/blockstore/libs/rdma/iface/public.h>
#include <cloud/blockstore/libs/storage/api/disk_registry.h>
#include <cloud/blockstore/libs/storage/api/partition.h>
#include <cloud/blockstore/libs/storage/api/service.h>
#include <cloud/blockstore/libs/storage/api/volume.h>
#include <cloud/blockstore/libs/storage/core/disk_counters.h>
#include <cloud/blockstore/libs/storage/core/request_info.h>
#include <cloud/blockstore/libs/storage/model/requests_in_progress.h>
#include <cloud/blockstore/libs/storage/partition_common/drain_actor_companion.h>
#include <cloud/blockstore/libs/storage/partition_common/get_changed_blocks_companion.h>
#include <cloud/storage/core/libs/common/compressed_bitmap.h>

#include <contrib/ydb/library/actors/core/actor_bootstrapped.h>
#include <contrib/ydb/library/actors/core/events.h>
#include <contrib/ydb/library/actors/core/hfunc.h>
#include <contrib/ydb/library/actors/core/mon.h>

namespace NCloud::NBlockStore::NStorage {

class TIncompleteMirrorRWModeControllerActor final
    : public NActors::TActorBootstrapped<TIncompleteMirrorRWModeControllerActor>
{
private:
    const TStorageConfigPtr Config;
    const TNonreplicatedPartitionConfigPtr PartConfig;
    const IProfileLogPtr ProfileLog;
    const IBlockDigestGeneratorPtr BlockDigestGenerator;
    const TString RwClientId;
    const NActors::TActorId PartNonreplActorId;
    const NActors::TActorId StatActorId;
    const NActors::TActorId MirrorPartitionActor;

    enum class EAgentState
    {
        Unavailable,
        Resyncing
    };
    struct TAgentState
    {
        EAgentState State;
        NActors::TActorId AgentAvailabilityWaiter;
        NActors::TActorId SmartResyncActor;
        std::shared_ptr<TCompressedBitmap> CleanBlocksMap;
    };
    THashMap<TString, TAgentState> AgentState;

    // ?
    TDynBitMap DirtyBlockMap;

public:
    TIncompleteMirrorRWModeControllerActor(
        TStorageConfigPtr config,
        TNonreplicatedPartitionConfigPtr partConfig,
        IProfileLogPtr profileLog,
        IBlockDigestGeneratorPtr blockDigestGenerator,
        TString rwClientId,
        NActors::TActorId partNonreplActorId,
        NActors::TActorId statActorId,
        NActors::TActorId mirrorPartitionActor);

    ~TIncompleteMirrorRWModeControllerActor() override;

    void Bootstrap(const NActors::TActorContext& ctx);

private:
    [[nodiscard]] bool AgentIsUnavailable(const TString& agentId) const;

    void TrimRequest(
        const TEvService::TEvWriteBlocksRequest::TPtr& ev,
        TBlockRange64 rangeToWrite,
        TBlockRange64 rangeToDelete,
        const TString& unavailableAgentId);
    void TrimRequest(
        const TEvService::TEvWriteBlocksLocalRequest::TPtr& ev,
        TBlockRange64 rangeToWrite,
        TBlockRange64 rangeToDelete,
        const TString& unavailableAgentId);
    void TrimRequest(
        const TEvService::TEvZeroBlocksRequest::TPtr& ev,
        TBlockRange64 rangeToWrite,
        TBlockRange64 rangeToDelete,
        const TString& unavailableAgentId);

    void MarkBlocksAsDirty(
        const TString& unavailableAgentId,
        TBlockRange64 range);

private:
    STFUNC(StateWork);
    STFUNC(StateZombie);

    void HandleAgentIsUnavailable(
        const NPartition::TEvPartition::TEvAgentIsUnavailable::TPtr& ev,
        const NActors::TActorContext& ctx);

    void HandleAgentIsBackOnline(
        const NPartition::TEvPartition::TEvAgentIsBackOnline::TPtr& ev,
        const NActors::TActorContext& ctx);

    void HandleWriteOrZeroCompleted(
        const TEvNonreplPartitionPrivate::TEvWriteOrZeroCompleted::TPtr& ev,
        const NActors::TActorContext& ctx);

    void HandleEnterIncompleteMirrorRWMode(
        const NPartition::TEvPartition::TEvEnterIncompleteMirrorRWModeRequest::
            TPtr& ev,
        const NActors::TActorContext& ctx);

    void HandlePoisonPill(
        const NActors::TEvents::TEvPoisonPill::TPtr& ev,
        const NActors::TActorContext& ctx);

    // void HandlePoisonTaken(
    //     const NActors::TEvents::TEvPoisonTaken::TPtr& ev,
    //     const NActors::TActorContext& ctx);

    template <typename TMethod>
    void WriteRequest(
        const typename TMethod::TRequest::TPtr& ev,
        const NActors::TActorContext& ctx);

    template <typename TMethod>
    void ReadBlocks(
        const typename TMethod::TRequest::TPtr& ev,
        const NActors::TActorContext& ctx);

    void ForwardUnexpectedEvent(
        TAutoPtr<::NActors::IEventHandle>& ev,
        const NActors::TActorContext& ctx);

    BLOCKSTORE_IMPLEMENT_REQUEST(WriteBlocks, TEvService);
    BLOCKSTORE_IMPLEMENT_REQUEST(WriteBlocksLocal, TEvService);
    BLOCKSTORE_IMPLEMENT_REQUEST(ZeroBlocks, TEvService);
    // BLOCKSTORE_IMPLEMENT_REQUEST(Drain, NPartition::TEvPartition);
    // ?????????????????????
};

}   // namespace NCloud::NBlockStore::NStorage
