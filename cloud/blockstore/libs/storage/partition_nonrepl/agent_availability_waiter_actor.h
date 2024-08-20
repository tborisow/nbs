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

#include <contrib/ydb/library/actors/core/actor_bootstrapped.h>
#include <contrib/ydb/library/actors/core/events.h>
#include <contrib/ydb/library/actors/core/hfunc.h>
#include <contrib/ydb/library/actors/core/mon.h>

namespace NCloud::NBlockStore::NStorage {

class TAgentAvailabilityWaiterActor final
    : public NActors::TActorBootstrapped<TAgentAvailabilityWaiterActor>
{
private:
    const TStorageConfigPtr Config;
    const TNonreplicatedPartitionConfigPtr PartConfig;
    const NActors::TActorId PartNonreplActorId;
    const NActors::TActorId ParentActorId;
    const NActors::TActorId StatActorId;
    const TString AgentId;

    ui64 ReadBlockIndex = 0;

public:
    TAgentAvailabilityWaiterActor(
        TStorageConfigPtr config,
        TNonreplicatedPartitionConfigPtr partConfig,
        NActors::TActorId partNonreplActorId,
        NActors::TActorId parentActorId,
        NActors::TActorId statActorId,
        TString agentId);

    ~TAgentAvailabilityWaiterActor() override;

    void Bootstrap(const NActors::TActorContext& ctx);

private:
    void ScheduleCheckAvailabilityRequest(const NActors::TActorContext& ctx);

private:
    STFUNC(StateWork);

    void HandleReadBlocks(
        const TEvService::TEvReadBlocksRequest::TPtr& ev,
        const NActors::TActorContext& ctx);

    void HandleReadBlocksResponse(
        const TEvService::TEvReadBlocksResponse::TPtr& ev,
        const NActors::TActorContext& ctx);

    void HandleWakeup(
        const NActors::TEvents::TEvWakeup::TPtr& ev,
        const NActors::TActorContext& ctx);

    void HandlePoisonPill(
        const NActors::TEvents::TEvPoisonPill::TPtr& ev,
        const NActors::TActorContext& ctx);

};

}   // namespace NCloud::NBlockStore::NStorage
