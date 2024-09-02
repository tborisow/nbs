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

////////////////////////////////////////////////////////////////////////////////
namespace {

using namespace NActors;

using namespace NKikimr;

template <typename TEvent>
void ForwardMessageToActor(
    TEvent& ev,
    const NActors::TActorContext& ctx,
    NActors::TActorId destActor)
{
    NActors::TActorId nondeliveryActor = ev->GetForwardOnNondeliveryRecipient();
    auto message = std::make_unique<IEventHandle>(
        destActor,
        ev->Sender,
        ev->ReleaseBase().Release(),
        ev->Flags,
        ev->Cookie,
        ev->Flags & NActors::IEventHandle::FlagForwardOnNondelivery
            ? &nondeliveryActor
            : nullptr);
    ctx.Send(std::move(message));
}

}   // namespace

////////////////////////////////////////////////////////////////////////////////

TIncompleteMirrorRWModeControllerActor::TIncompleteMirrorRWModeControllerActor(
        TStorageConfigPtr config,
        TNonreplicatedPartitionConfigPtr partConfig,
        IProfileLogPtr profileLog,
        IBlockDigestGeneratorPtr blockDigestGenerator,
        TString rwClientId,
        TActorId partNonreplActorId,
        TActorId statActorId,
        TActorId mirrorPartitionActor)
    : Config(std::move(config))
    , PartConfig(std::move(partConfig))
    , ProfileLog(std::move(profileLog))
    , BlockDigestGenerator(std::move(blockDigestGenerator))
    , RwClientId(std::move(rwClientId))
    , PartNonreplActorId(partNonreplActorId)
    , StatActorId(statActorId)
    , MirrorPartitionActor(mirrorPartitionActor)
{}

TIncompleteMirrorRWModeControllerActor::~TIncompleteMirrorRWModeControllerActor() = default;

void TIncompleteMirrorRWModeControllerActor::Bootstrap(const TActorContext& ctx)
{
    Y_UNUSED(ctx);
    Become(&TThis::StateWork);
}

void TIncompleteMirrorRWModeControllerActor::HandleAgentIsUnavailable(
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
        state.CleanBlocksMap = std::make_shared<TCompressedBitmap>(PartConfig->GetBlockCount());
        state.CleanBlocksMap->Set(0, PartConfig->GetBlockCount());

        // ui64 blockIndex = 0;
        // const auto& devices = PartConfig->GetDevices();
        // for (const auto& device: devices) {
        //     if (device.GetAgentId() != msg->AgentId) {
        //         // Skip this device for migration
        //         state.CleanBlockMap->Set(
        //             blockIndex,
        //             blockIndex + device.GetBlocksCount() - 1);
        //     }
        //     blockIndex += device.GetBlocksCount();
        // }

        if (state.SmartResyncActor) {
            NCloud::Send<TEvents::TEvPoisonPill>(ctx, state.SmartResyncActor);
            state.SmartResyncActor = TActorId();
        }
    } else {
        Y_DEBUG_ABORT_UNLESS(AgentState[msg->AgentId].AgentAvailabilityWaiter);
        Y_DEBUG_ABORT_UNLESS(!AgentState[msg->AgentId].SmartResyncActor);
    }
}

void TIncompleteMirrorRWModeControllerActor::HandleAgentIsBackOnline(
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

bool TIncompleteMirrorRWModeControllerActor::AgentIsUnavailable(
    const TString& agentId) const
{
    const TAgentState* state = AgentState.FindPtr(agentId);
    return state && state->State == EAgentState::Unavailable;
}

void TIncompleteMirrorRWModeControllerActor::MarkBlocksAsDirty(
    const TString& unavailableAgentId,
    TBlockRange64 range)
{
    Y_DEBUG_ABORT_UNLESS(AgentState.contains(unavailableAgentId));
    auto& state = AgentState[unavailableAgentId];
    Y_DEBUG_ABORT_UNLESS(state.CleanBlocksMap);
    state.CleanBlocksMap->Unset(range.Start, range.End);
}


void TIncompleteMirrorRWModeControllerActor::TrimRequest(
    const TEvService::TEvWriteBlocksRequest::TPtr& ev,
    TBlockRange64 rangeToWrite,
    TBlockRange64 rangeToDelete,
    const TString& unavailableAgentId)
{
    auto& request = ev->Get()->Record;
    Y_DEBUG_ABORT_UNLESS(rangeToWrite.Size() > 0);
    Y_DEBUG_ABORT_UNLESS(rangeToDelete.Size() > 0);
    Y_DEBUG_ABORT_UNLESS(!unavailableAgentId.empty());

    const ui64 oldStartIndex = request.GetStartIndex();
    request.SetStartIndex(rangeToWrite.Start);
    request.MutableBlocks()->MutableBuffers()->DeleteSubrange(rangeToDelete.Start - oldStartIndex, rangeToDelete.Size());

    Y_DEBUG_ABORT_UNLESS(request.GetBlocks().GetBuffers().size() > 0);
    Y_DEBUG_ABORT_UNLESS(request.GetBlocks().GetBuffers().size() == static_cast<int>(rangeToWrite.Size()));

    MarkBlocksAsDirty(unavailableAgentId, rangeToWrite);
}

void TIncompleteMirrorRWModeControllerActor::TrimRequest(
    const TEvService::TEvWriteBlocksLocalRequest::TPtr& ev,
    TBlockRange64 rangeToWrite,
    TBlockRange64 rangeToDelete,
    const TString& unavailableAgentId)
{
    auto* msg = ev->Get();
    const ui64 oldStartIndex = msg->Record.GetStartIndex();
    msg->Record.SetStartIndex(rangeToWrite.Start);

    auto guard = msg->Record.Sglist.Acquire();
    const auto& list = guard.Get();

    auto partialList2 = TSgList();
    ui64 blockIndex = oldStartIndex;
    for (const TBlockDataRef& block: list) {
        if (blockIndex >= rangeToWrite.Start && blockIndex < rangeToWrite.End) {
            Y_DEBUG_ABORT_UNLESS(
                blockIndex < rangeToDelete.Start ||
                blockIndex > rangeToDelete.End);
            partialList2.push_back(block);
        }
        blockIndex += block.Size() / msg->Record.BlockSize;
    }

    MarkBlocksAsDirty(unavailableAgentId, rangeToWrite);
}

void TIncompleteMirrorRWModeControllerActor::TrimRequest(
    const TEvService::TEvZeroBlocksRequest::TPtr& ev,
    TBlockRange64 rangeToWrite,
    TBlockRange64 rangeToDelete,
    const TString& unavailableAgentId)
{
    auto& request = ev->Get()->Record;
    Y_DEBUG_ABORT_UNLESS(rangeToWrite.Size() > 0);
    Y_DEBUG_ABORT_UNLESS(rangeToDelete.Size() > 0);
    Y_DEBUG_ABORT_UNLESS(!unavailableAgentId.empty());

    request.SetStartIndex(rangeToWrite.Start);
    request.SetBlocksCount(rangeToWrite.Size());

    MarkBlocksAsDirty(unavailableAgentId, rangeToWrite);
}

// void TIncompleteMirrorRWModeControllerActor::HandleReadBlocks(
//     const TEvService::TEvReadBlocksRequest::TPtr& ev,
//     const TActorContext& ctx)
// {
//     ReadBlocks<TEvService::TReadBlocksMethod>(ev, ctx);
// }

// void TIncompleteMirrorRWModeControllerActor::HandleReadBlocksLocal(
//     const TEvService::TEvReadBlocksLocalRequest::TPtr& ev,
//     const TActorContext& ctx)
// {
//     ReadBlocks<TEvService::TReadBlocksLocalMethod>(ev, ctx);
// }

template <typename TMethod>
void TIncompleteMirrorRWModeControllerActor::WriteRequest(
    const typename TMethod::TRequest::TPtr& ev,
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
        for (const auto& deviceRequest: deviceRequests) {
            Y_DEBUG_ABORT_UNLESS(
                AgentState.contains(deviceRequest.Device.GetAgentId()));
            auto& state = AgentState[deviceRequest.Device.GetAgentId()];
            Y_DEBUG_ABORT_UNLESS(state.CleanBlocksMap);

            state.CleanBlocksMap->Unset(
                deviceRequest.BlockRange.Start,
                deviceRequest.BlockRange.End);
        }

        // Repond with fake "S_OK".
        auto response = std::make_unique<TEvService::TEvWriteBlocksResponse>();
        NCloud::Reply(ctx, *ev, std::move(response));
        return;
    }

    using TRequest = TMethod::TRequest;
    auto request = std::make_unique<TRequest>();
    request->Record = std::move(msg->Record);
    request->CallContext = msg->CallContext;
    if (deviceRequests.size() == 2 && availableAgentCount == 1) {
        // In this case we should discard blocks which would be written in the unavailable agent.
        Y_DEBUG_ABORT_UNLESS(requestBlockCount == rangeToDelete.Size() + rangeToWrite.Size());
        TrimRequest(ev, rangeToWrite, rangeToDelete, unavailableAgentId);
    }

    // Who will be recipient of ok request: SmartResyncActor or PartNonreplActorId ?
    ctx.Send(std::make_unique<IEventHandle>(
        PartNonreplActorId,
        ev->Sender,
        request.release(),
        ev->Flags,
        ev->Cookie,
        &ev->Sender));
}

void TIncompleteMirrorRWModeControllerActor::HandleWriteBlocks(
    const TEvService::TEvWriteBlocksRequest::TPtr& ev,
    const TActorContext& ctx)
{
    WriteRequest<TEvService::TWriteBlocksMethod>(ev, ctx);
    auto request = std::make_unique<TEvService::TWriteBlocksMethod::TRequest>();
}

void TIncompleteMirrorRWModeControllerActor::HandleWriteBlocksLocal(
    const TEvService::TEvWriteBlocksLocalRequest::TPtr& ev,
    const TActorContext& ctx)
{
    WriteRequest<TEvService::TWriteBlocksLocalMethod>(ev, ctx);
}

void TIncompleteMirrorRWModeControllerActor::HandleZeroBlocks(
    const TEvService::TEvZeroBlocksRequest::TPtr& ev,
    const TActorContext& ctx)
{
    WriteRequest<TEvService::TZeroBlocksMethod>(ev, ctx);
}

// void TIncompleteMirrorRWModeControllerActor::HandleDrain(
//     const NPartition::TEvPartition::TEvDrainRequest::TPtr& ev,
//     const NActors::TActorContext& ctx)
// {
//     ForwardMessageToActor(ev, ctx, PartNonreplActorId);
// }

void TIncompleteMirrorRWModeControllerActor::HandlePoisonPill(
    const TEvents::TEvPoisonPill::TPtr& ev,
    const TActorContext& ctx)
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


STFUNC(TIncompleteMirrorRWModeControllerActor::StateWork)
{
    switch (ev->GetTypeRewrite()) {
        // HFunc(TEvService::TEvReadBlocksRequest, HandleReadBlocks);
        HFunc(TEvService::TEvWriteBlocksRequest, HandleWriteBlocks);
        HFunc(TEvService::TEvWriteBlocksLocalRequest, HandleWriteBlocksLocal);
        HFunc(TEvService::TEvZeroBlocksRequest, HandleZeroBlocks);

        HFunc(NPartition::TEvPartition::TEvAgentIsUnavailable, HandleAgentIsUnavailable);
        HFunc(NPartition::TEvPartition::TEvAgentIsBackOnline, HandleAgentIsBackOnline);
        // HFunc(NPartition::TEvPartition::TEvDrainRequest, HandleDrain);

        // HFunc(TEvService::TEvReadBlocksLocalRequest, HandleReadBlocksLocal);

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
            // NOTE: We handle all unexpected requests and proxy them to
            // nonreplicated partititon.
            ForwardUnexpectedEvent(ev, ActorContext());
            break;
    }
}

void TIncompleteMirrorRWModeControllerActor::ForwardUnexpectedEvent(
    TAutoPtr<::NActors::IEventHandle>& ev,
    const TActorContext& ctx)
{
    ForwardMessageToActor(ev, ctx, PartNonreplActorId);
}

STFUNC(TIncompleteMirrorRWModeControllerActor::StateZombie)
{
    switch (ev->GetTypeRewrite()) {
        IgnoreFunc(TEvNonreplPartitionPrivate::TEvUpdateCounters);

        IgnoreFunc(TEvNonreplPartitionPrivate::TEvScrubbingNextRange);
        IgnoreFunc(TEvNonreplPartitionPrivate::TEvChecksumBlocksRequest);
        IgnoreFunc(TEvNonreplPartitionPrivate::TEvChecksumBlocksResponse);

        // HFunc(TEvService::TEvReadBlocksRequest, RejectReadBlocks);
        HFunc(TEvService::TEvWriteBlocksRequest, RejectWriteBlocks);
        HFunc(TEvService::TEvZeroBlocksRequest, RejectZeroBlocks);
        HFunc(TEvService::TEvWriteBlocksLocalRequest, RejectWriteBlocksLocal);

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
        // HFunc(TEvents::TEvPoisonTaken, HandlePoisonTaken);

        default:
            HandleUnexpectedEvent(ev, TBlockStoreComponents::PARTITION);
            break;
    }
}

}   // namespace NCloud::NBlockStore::NStorage
