#include "incomplete_mirror_rw_mode_controller_actor.h"

#include "agent_availability_waiter_actor.h"

#include <cloud/blockstore/libs/diagnostics/critical_events.h>
#include <cloud/blockstore/libs/service/request_helpers.h>
#include <cloud/blockstore/libs/storage/core/config.h>
#include <cloud/blockstore/libs/storage/core/forward_helpers.h>
#include <cloud/blockstore/libs/storage/core/probes.h>
#include <cloud/blockstore/libs/storage/core/proto_helpers.h>
#include <cloud/blockstore/libs/storage/core/unimplemented.h>

#include <contrib/ydb/core/base/appdata.h>

namespace NCloud::NBlockStore::NStorage {

using namespace NActors;

using namespace NKikimr;

using TEvPartition = NPartition::TEvPartition;

LWTRACE_USING(BLOCKSTORE_STORAGE_PROVIDER);

TSmartResyncActor::TSmartResyncActor(
        TStorageConfigPtr config,
        TNonreplicatedPartitionConfigPtr partConfig,
        NActors::TActorId partNonreplActorId,
        TActorId statActorId)
    : Config(std::move(config))
    , PartConfig(std::move(partConfig))
    , PartNonreplActorId(partNonreplActorId)
    , StatActorId(statActorId)
{
    partConfig->Get
}

TSmartResyncActor::~TSmartResyncActor() = default;

template <typename TMethod>
void TSmartResyncActor::WriteRequest(
    const typename TMethod::TRequest::TPtr& ev,
    const TActorContext& ctx)
{
    // Either send it or return success and mark the block.
    // If we send it, then we may be need to break it into two pieces.


    auto requestInfo = CreateRequestInfo(
        ev->Sender,
        ev->Cookie,
        ev->Get()->CallContext);

    auto& record = ev->Get()->Record;
    // const auto blockRange = TBlockRange64::WithLength(
    //     record.GetStartIndex(),
    //     record.GetBlocksCount());

    auto request = std::make_unique<typename TMethod::TRequest>();
    request->CallContext = requestInfo->CallContext;
    request->Record = std::move(record);

    auto event = std::make_unique<IEventHandle>(
        PartNonreplActorId,
        requestInfo->Sender,
        request.release(),
        NActors::IEventHandle::FlagForwardOnNondelivery,
        requestInfo->Cookie,
        &requestInfo->Sender   // forwardOnNondelivery ???????????????????
    );

    ctx.Send(event.release());
}

void TSmartResyncActor::OnBootstrap(const TActorContext& ctx)
{
    Y_UNUSED(ctx);
    Become(&TThis::StateWork);
}

void TSmartResyncActor::HandleAgentIsUnavailable(
    const NPartition::TEvPartition::TEvAgentIsUnavailable::TPtr& ev,
    const NActors::TActorContext& ctx)
{
    const auto* msg = ev->Get();

    TAgentState* state = AgentState.FindPtr(msg->AgentId);
    if (!state || state->State == EAgentState::Resyncing) {
        auto waiterActorId = NCloud::Register(
                ctx,
                std::make_unique<TAgentAvailabilityWaiterActor>(
                    Config,
                    PartConfig,
                    PartNonreplActorId,
                    SelfId(),
                    StatActorId, msg->AgentId));
        auto& state = AgentState[msg->AgentId];
        state.State = EAgentState::Unavailable;
        state.AgentAvailabilityWaiter = waiterActorId;

        if (state.SmartResyncActor) {
            NCloud::Send<TEvents::TEvPoisonPill>(ctx, state.SmartResyncActor);
            state.SmartResyncActor = TActorId();
        }
    } else {
        Y_DEBUG_ABORT_UNLESS(AgentState[msg->AgentId].AgentAvailabilityWaiter);
        Y_DEBUG_ABORT_UNLESS(!AgentState[msg->AgentId].SmartResyncActor);
    }
}

void TSmartResyncActor::HandleAgentIsBackOnline(
        const NPartition::TEvPartition::TEvAgentIsBackOnline::TPtr& ev,
        const NActors::TActorContext& ctx) {
    const auto* msg = ev->Get();

    Y_DEBUG_ABORT_UNLESS(AgentState.contains(msg->AgentId));
    if (!AgentState.contains(msg->AgentId)) {
        return;
    }

    switch(AgentState[msg->AgentId].State) {
        case EAgentState::Unavailable: {
            TAgentState& state = AgentState[msg->AgentId];
            Y_DEBUG_ABORT_UNLESS(!state.SmartResyncActor);
            Y_DEBUG_ABORT_UNLESS(state.AgentAvailabilityWaiter);

            NCloud::Send<TEvents::TEvPoisonPill>(ctx, state.AgentAvailabilityWaiter);
            state.AgentAvailabilityWaiter = TActorId();
            state.SmartResyncActor = TActorId();   //             SET NEW ACTOR HERE!
            state.State = EAgentState::Resyncing;
            break;
        }
        case EAgentState::Resyncing: {
            const TAgentState& state = AgentState[msg->AgentId];
            Y_DEBUG_ABORT_UNLESS(state.SmartResyncActor);
            Y_DEBUG_ABORT_UNLESS(!state.AgentAvailabilityWaiter);
            // no-op?
            break;
        }
    }

}

bool TSmartResyncActor::AgentIsUnavailable(
    const TString& agentId) const
{
    const TAgentState* state = AgentState.FindPtr(agentId);
    return state && state->State == EAgentState::Unavailable;
}

// void TSmartResyncActor::HandleReadBlocks(
//     const TEvService::TEvReadBlocksRequest::TPtr& ev,
//     const TActorContext& ctx)
// {
//     ReadBlocks<TEvService::TReadBlocksMethod>(ev, ctx);
// }

// void TSmartResyncActor::HandleReadBlocksLocal(
//     const TEvService::TEvReadBlocksLocalRequest::TPtr& ev,
//     const TActorContext& ctx)
// {
//     ReadBlocks<TEvService::TReadBlocksLocalMethod>(ev, ctx);
// }

void TSmartResyncActor::HandleWriteBlocks(
    const TEvService::TEvWriteBlocksRequest::TPtr& ev,
    const TActorContext& ctx)
{
    auto* msg = ev->Get();

    const ui32 requestBlockCount = CalculateWriteRequestBlockCount(
        msg->Record,
        PartConfig->GetBlockSize());
    const auto blockRange = TBlockRange64::WithLength(
        msg->Record.GetStartIndex(),
        requestBlockCount);
    auto deviceRequests = PartConfig->ToDeviceRequests(blockRange);

    // The code down below relies on this.
    Y_DEBUG_ABORT_UNLESS(deviceRequests.size() <= 2);

    TBlockRange64 rangeToWrite;
    TBlockRange64 rangeToDelete;
    size_t availableAgentCount = 0;
    TString unavailableAgentId;
    for (const auto& deviceRequest: deviceRequests) {
        if (AgentIsUnavailable(deviceRequest.Device.GetAgentId())) {
            rangeToDelete = deviceRequest.BlockRange;
            unavailableAgentId = deviceRequest.Device.GetAgentId();
        } else {
            availableAgentCount++;
            rangeToWrite = deviceRequest.BlockRange;
        }
    }

    if (availableAgentCount == 0) {
        // TODO: Mark the blocks and return success to mirror3 partition.
        // for (const auto& deviceRequest: deviceRequests) {
        //     AgentState[]
        // }
        return;
    }

    auto request = std::make_unique<TEvService::TEvWriteBlocksRequest>();
    request->Record = std::move(msg->Record);
    request->CallContext = msg->CallContext;
    // In this case we should discard blocks which would be written in the unavailable agent.
    if (deviceRequests.size() == 2 && availableAgentCount == 1) {
        Y_DEBUG_ABORT_UNLESS(requestBlockCount == rangeToDelete.Size() + rangeToWrite.Size());
        Y_DEBUG_ABORT_UNLESS(rangeToWrite.Size() > 0);
        Y_DEBUG_ABORT_UNLESS(rangeToDelete.Size() > 0);
        Y_DEBUG_ABORT_UNLESS(!unavailableAgentId.empty());

        const ui64 oldStartIndex = request->Record.GetStartIndex();
        request->Record.SetStartIndex(rangeToWrite.Start);
        request->Record.MutableBlocks()->MutableBuffers()->DeleteSubrange(rangeToDelete.Start - oldStartIndex, rangeToDelete.Size());

        Y_DEBUG_ABORT_UNLESS(request->Record.GetBlocks().GetBuffers().size() > 0);
        Y_DEBUG_ABORT_UNLESS(request->Record.GetBlocks().GetBuffers().size() == static_cast<int>(rangeToWrite.Size()));

        // TODO: MARK DELETED BLOCKS!
    }

    ctx.Send(std::make_unique<IEventHandle>(
        PartNonreplActorId,
        ev->Sender,
        request.release(),
        ev->Flags,
        ev->Cookie,
        &ev->Sender));
}

void TSmartResyncActor::HandleWriteBlocksLocal(
    const TEvService::TEvWriteBlocksLocalRequest::TPtr& ev,
    const TActorContext& ctx)
{
    auto* msg = ev->Get();

    const ui32 requestBlockCount = CalculateWriteRequestBlockCount(
        msg->Record,
        PartConfig->GetBlockSize());
    const auto blockRange = TBlockRange64::WithLength(
        msg->Record.GetStartIndex(),
        requestBlockCount);
    auto deviceRequests = PartConfig->ToDeviceRequests(blockRange);

    // The code down below relies on this.
    Y_DEBUG_ABORT_UNLESS(deviceRequests.size() <= 2);

    TBlockRange64 rangeToWrite;
    TBlockRange64 rangeToDelete;
    size_t availableAgentCount = 0;
    for (const auto& deviceRequest: deviceRequests) {
        if (AgentIsUnavailable(deviceRequest.Device.GetAgentId())) {
            rangeToDelete = deviceRequest.BlockRange;
        } else {
            availableAgentCount++;
            rangeToWrite = deviceRequest.BlockRange;
        }
    }

    auto request = std::make_unique<TEvService::TEvWriteBlocksLocalRequest>();
    request->Record = std::move(msg->Record);
    request->CallContext = msg->CallContext;

    if (deviceRequests.size() > 1 &&
        availableAgentCount != deviceRequests.size())
    {
        const ui64 oldStartIndex = request->Record.GetStartIndex();
        request->Record.SetStartIndex(rangeToWrite.Start);

        auto guard = msg->Record.Sglist.Acquire();
        const auto& list = guard.Get();

        auto partialList = TSgList(
            list.cbegin() + (rangeToWrite.Start - oldStartIndex),
            list.cbegin() + rangeToWrite.Size());
        request->Record.Sglist =
            request->Record.Sglist.Create(std::move(partialList));
    }

    ctx.Send(std::make_unique<IEventHandle>(
        PartNonreplActorId,
        ev->Sender,
        request.release(),
        ev->Flags,
        ev->Cookie,
        &ev->Sender));
}

void TSmartResyncActor::HandleZeroBlocks(
    const TEvService::TEvZeroBlocksRequest::TPtr& ev,
    const TActorContext& ctx)
{
    // TODO(komarevtsev): IMPLEMENT


    ctx.Send(std::make_unique<IEventHandle>(
        PartNonreplActorId,
        ev->Sender,
        ev->ReleaseBase().Release(),
        ev->Flags,
        ev->Cookie,
        &ev->Sender));
}

void TSmartResyncActor::HandlePoisonPill(
    const NActors::TEvents::TEvPoisonPill::TPtr& ev,
    const NActors::TActorContext& ctx)
{
    Y_UNUSED(ev);

    for (const auto& state: AgentState) {
        NCloud::Send<TEvents::TEvPoisonPill>(
            ctx,
            state.second.AgentAvailabilityWaiter);
        NCloud::Send<TEvents::TEvPoisonPill>(
            ctx,
            state.second.SmartResyncActor);
    }
    AgentState.clear();
    Die(ctx);
}

////////////////////////////////////////////////////////////////////////////////


STFUNC(TSmartResyncActor::StateWork)
{
    switch (ev->GetTypeRewrite()) {
        // HFunc(TEvService::TEvReadBlocksRequest, HandleReadBlocks);
        HFunc(TEvService::TEvWriteBlocksRequest, HandleWriteBlocks);
        HFunc(TEvService::TEvZeroBlocksRequest, HandleZeroBlocks);
        HFunc(NPartition::TEvPartition::TEvAgentIsUnavailable, HandleAgentIsUnavailable);
        HFunc(NPartition::TEvPartition::TEvAgentIsBackOnline, HandleAgentIsBackOnline);

        // HFunc(TEvService::TEvReadBlocksLocalRequest, HandleReadBlocksLocal);
        // HFunc(TEvService::TEvWriteBlocksLocalRequest, HandleWriteBlocksLocal);

        // HFunc(TEvPartition::TEvDrainRequest, DrainActorCompanion.HandleDrain);
        // HFunc(TEvPartition::TEvEnterIncompleteMirrorRWModeRequest, HandleEnterIncompleteMirrorRWMode);

        // HFunc(
        //     TEvService::TEvGetChangedBlocksRequest,
        //     GetChangedBlocksCompanion.HandleGetChangedBlocks);

        // HFunc(TEvVolume::TEvDescribeBlocksRequest, HandleDescribeBlocks);
        // HFunc(TEvVolume::TEvGetCompactionStatusRequest, HandleGetCompactionStatus);
        // HFunc(TEvVolume::TEvCompactRangeRequest, HandleCompactRange);
        // HFunc(TEvVolume::TEvRebuildMetadataRequest, HandleRebuildMetadata);
        // HFunc(TEvVolume::TEvGetRebuildMetadataStatusRequest, HandleGetRebuildMetadataStatus);
        // HFunc(TEvVolume::TEvScanDiskRequest, HandleScanDisk);
        // HFunc(TEvVolume::TEvGetScanDiskStatusRequest, HandleGetScanDiskStatus);

        // HFunc(
        //     TEvNonreplPartitionPrivate::TEvWriteOrZeroCompleted,
        //     HandleWriteOrZeroCompleted);

        // HFunc(
        //     TEvVolume::TEvRWClientIdChanged,
        //     HandleRWClientIdChanged);
        // HFunc(
        //     TEvVolume::TEvDiskRegistryBasedPartitionCounters,
        //     HandlePartCounters);

        HFunc(TEvents::TEvPoisonPill, HandlePoisonPill);

        default:
            HandleUnexpectedEvent(ev, TBlockStoreComponents::PARTITION);
            break;
    }
}

STFUNC(TSmartResyncActor::StateZombie)
{
    switch (ev->GetTypeRewrite()) {
        IgnoreFunc(TEvNonreplPartitionPrivate::TEvUpdateCounters);

        IgnoreFunc(TEvNonreplPartitionPrivate::TEvScrubbingNextRange);
        IgnoreFunc(TEvNonreplPartitionPrivate::TEvChecksumBlocksRequest);
        IgnoreFunc(TEvNonreplPartitionPrivate::TEvChecksumBlocksResponse);

        // HFunc(TEvService::TEvReadBlocksRequest, RejectReadBlocks);
        HFunc(TEvService::TEvWriteBlocksRequest, RejectWriteBlocks);
        HFunc(TEvService::TEvZeroBlocksRequest, RejectZeroBlocks);

        // HFunc(TEvService::TEvReadBlocksLocalRequest, RejectReadBlocksLocal);
        // HFunc(TEvService::TEvWriteBlocksLocalRequest, RejectWriteBlocksLocal);

        // HFunc(NPartition::TEvPartition::TEvDrainRequest, RejectDrain);

        // HFunc(TEvVolume::TEvDescribeBlocksRequest, RejectDescribeBlocks);
        // HFunc(TEvVolume::TEvGetCompactionStatusRequest, RejectGetCompactionStatus);
        // HFunc(TEvVolume::TEvCompactRangeRequest, RejectCompactRange);
        // HFunc(TEvVolume::TEvRebuildMetadataRequest, RejectRebuildMetadata);
        // HFunc(TEvVolume::TEvGetRebuildMetadataStatusRequest, RejectGetRebuildMetadataStatus);
        // HFunc(TEvVolume::TEvScanDiskRequest, RejectScanDisk);
        // HFunc(TEvVolume::TEvGetScanDiskStatusRequest, RejectGetScanDiskStatus);

        // IgnoreFunc(TEvNonreplPartitionPrivate::TEvWriteOrZeroCompleted);

        // IgnoreFunc(TEvVolume::TEvRWClientIdChanged);
        // IgnoreFunc(TEvVolume::TEvDiskRegistryBasedPartitionCounters);

        IgnoreFunc(TEvents::TEvPoisonPill);
        HFunc(TEvents::TEvPoisonTaken, HandlePoisonTaken);

        default:
            HandleUnexpectedEvent(ev, TBlockStoreComponents::PARTITION);
            break;
    }
}

}   // namespace NCloud::NBlockStore::NStorage
