#include "incomplete_mirror_rw_mode_controller_actor.h"

#include <cloud/blockstore/libs/diagnostics/critical_events.h>
#include <cloud/blockstore/libs/service/request_helpers.h>
#include <cloud/blockstore/libs/storage/core/config.h>
#include <cloud/blockstore/libs/storage/core/forward_helpers.h>
#include <cloud/blockstore/libs/storage/core/probes.h>
#include <cloud/blockstore/libs/storage/core/proto_helpers.h>
#include <cloud/blockstore/libs/storage/core/unimplemented.h>
#include <cloud/blockstore/libs/storage/partition_nonrepl/agent_availability_waiter_actor.h>
#include <cloud/blockstore/libs/storage/partition_nonrepl/part_nonrepl_common.h>

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

template <typename TMethod>
class TSplitRequestSenderActor final
    : public NActors::TActorBootstrapped<TSplitRequestSenderActor<TMethod>>
{
private:
    const TRequestInfoPtr RequestInfo;
    TVector<TSplitRequest> Requests;
    const TString DiskId;
    const NActors::TActorId ParentActorId;
    const ui64 RequestId;

    ui32 Responses = 0;
    typename TMethod::TResponse::ProtoRecordType Record;

    using TBase = NActors::TActorBootstrapped<TSplitRequestSenderActor<TMethod>>;

public:
    TSplitRequestSenderActor(
        TRequestInfoPtr requestInfo,
        TVector<TSplitRequest> requests,
        TString diskId,
        NActors::TActorId parentActorId,
        ui64 requestId);
    ~TSplitRequestSenderActor() override = default;

    void Bootstrap(const NActors::TActorContext& ctx);

private:
    void SendRequests(const NActors::TActorContext& ctx);
    void Done(const NActors::TActorContext& ctx);

private:
    STFUNC(StateWork);

    void HandleResponse(
        const typename TMethod::TResponse::TPtr& ev,
        const NActors::TActorContext& ctx);

    void HandlePoisonPill(
        const NActors::TEvents::TEvPoisonPill::TPtr& ev,
        const NActors::TActorContext& ctx);

    void HandleUndelivery(
        const typename TMethod::TRequest::TPtr& ev,
        const NActors::TActorContext& ctx);
};

template <typename TMethod>
TSplitRequestSenderActor<TMethod>::TSplitRequestSenderActor(
    TRequestInfoPtr requestInfo,
    TVector<TSplitRequest> requests,
    TString diskId,
    NActors::TActorId parentActorId,
    ui64 requestId)
    : RequestInfo(std::move(requestInfo))
    , Requests(std::move(requests))
    , DiskId(std::move(diskId))
    , ParentActorId(parentActorId)
    , RequestId(requestId)
{
    Y_DEBUG_ABORT_UNLESS(!requests.empty());
    Y_DEBUG_ABORT_UNLESS(!DiskId.empty());
    Y_DEBUG_ABORT_UNLESS(parentActorId);
    Y_DEBUG_ABORT_UNLESS(!DiskId.empty());
}

template <typename TMethod>
void TSplitRequestSenderActor<TMethod>::Bootstrap(const NActors::TActorContext& ctx) {
    TRequestScope timer(*RequestInfo);

    TBase::Become(&TBase::TThis::StateWork);

    LWTRACK(
        RequestReceived_PartitionWorker,
        RequestInfo->CallContext->LWOrbit,
        TMethod::Name,
        RequestInfo->CallContext->RequestId);

    SendRequests(ctx);
}

template <typename TMethod>
void TSplitRequestSenderActor<TMethod>::SendRequests(const NActors::TActorContext& ctx) {
    for (auto& request: Requests) {
        Y_DEBUG_ABORT_UNLESS(request.CallContext);

        auto event = std::make_unique<NActors::IEventHandle>(
            request.RecipientActorId,
            ctx.SelfID,
            request.Request.release(),
            NActors::IEventHandle::FlagForwardOnNondelivery,
            RequestInfo->Cookie,   // cookie
            &ctx.SelfID            // forwardOnNondelivery
        );

        ctx.Send(std::move(event));
    }
}

template <typename TMethod>
void TSplitRequestSenderActor<TMethod>::Done(const NActors::TActorContext& ctx)
{
    auto response = std::make_unique<typename TMethod::TResponse>();
    response->Record = std::move(Record);

    auto& callContext = *RequestInfo->CallContext;
    for (auto& request: Requests) {
        callContext.LWOrbit.Join(request.CallContext->LWOrbit);
    }

    // LWTRACK(
    //     ResponseSent_PartitionWorker,
    //     RequestInfo->CallContext->LWOrbit,
    //     TMethod::Name,
    //     RequestInfo->CallContext->RequestId);


    NCloud::Reply(ctx, *RequestInfo, std::move(response));

    // TODO: NOT NEEDED, right?

    // using TCompletion =
    //     TEvNonreplPartitionPrivate::TEvWriteOrZeroCompleted;
    // auto completion =
    //     std::make_unique<TCompletion>(
    //         NonreplicatedRequestCounter,
    //         RequestInfo->GetTotalCycles());

    // NCloud::Send(ctx, ParentActorId, std::move(completion));

    TBase::Die(ctx);
}

////////////////////////////////////////////////////////////////////////////////

template <typename TMethod>
void TSplitRequestSenderActor<TMethod>::HandleUndelivery(
    const typename TMethod::TRequest::TPtr& ev,
    const NActors::TActorContext& ctx)
{
    Y_UNUSED(ev);

    LOG_WARN(ctx, TBlockStoreComponents::PARTITION_WORKER,
        "[%s] %s request undelivered to some nonrepl partitions",
        DiskId.c_str(),
        TMethod::Name);

    Record.MutableError()->CopyFrom(MakeError(E_REJECTED, TStringBuilder()
        << TMethod::Name << " request undelivered to some nonrepl partitions"));

    if (++Responses < Requests.size()) {
        return;
    }

    Done(ctx);
}

template <typename TMethod>
void TSplitRequestSenderActor<TMethod>::HandleResponse(
    const typename TMethod::TResponse::TPtr& ev,
    const NActors::TActorContext& ctx)
{
    auto* msg = ev->Get();

    if (HasError(msg->Record)) {
        LOG_ERROR(ctx, TBlockStoreComponents::PARTITION_WORKER,
            "[%s] %s got error from nonreplicated partition: %s",
            DiskId.c_str(),
            TMethod::Name,
            FormatError(msg->Record.GetError()).c_str());
    }

    if (!HasError(Record)) {
        Record = std::move(msg->Record);
    }

    if (++Responses < Requests.size()) {
        return;
    }

    Done(ctx);
}

template <typename TMethod>
void TSplitRequestSenderActor<TMethod>::HandlePoisonPill(
    const NActors::TEvents::TEvPoisonPill::TPtr& ev,
    const NActors::TActorContext& ctx)
{
    Y_UNUSED(ev);

    *Record.MutableError() =
        MakeError(E_REJECTED, "TSplitRequestSenderActor is dead");
    Done(ctx);
}

template <typename TMethod>
STFUNC(TSplitRequestSenderActor<TMethod>::StateWork)
{
    TRequestScope timer(*RequestInfo);

    switch (ev->GetTypeRewrite()) {
        HFunc(NActors::TEvents::TEvPoisonPill, HandlePoisonPill);

        HFunc(TMethod::TResponse, HandleResponse);
        HFunc(TMethod::TRequest, HandleUndelivery);

        default:
            HandleUnexpectedEvent(
                ev,
                TBlockStoreComponents::PARTITION_WORKER);
            break;
    }
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

TIncompleteMirrorRWModeControllerActor::
    ~TIncompleteMirrorRWModeControllerActor() = default;

void TIncompleteMirrorRWModeControllerActor::Bootstrap(const TActorContext& ctx)
{
    Y_UNUSED(ctx);
    Become(&TThis::StateWork);
}

bool TIncompleteMirrorRWModeControllerActor::ShouldSplitWriteRequest(
    const TVector<TDeviceRequest>& requests) const
{
    Y_DEBUG_ABORT_UNLESS(!requests.empty());

    TSet<TString> agents;
    for (const auto& request: requests) {
        agents.insert(request.Device.GetAgentId());
    }

    if (agents.size() == 1) {
        return false;
    }

    TVector<std::optional<TIncompleteMirrorRWModeControllerActor::EAgentState>>
        states;
    for (const auto& agent: agents) {
        if (AgentState.contains(agent)) {
            states.push_back(AgentState.at(agent).State);
        } else {
            states.push_back(std::nullopt);
        }
    }

    Unique(states.begin(), states.end());
    return states.size() != 1;
}

void TIncompleteMirrorRWModeControllerActor::OnMigrationProgress(
    const TString& agentId,
    ui64 processedBlockCount,
    ui64 blockCountNeedToBeProcessed)
{
    Y_DEBUG_ABORT_UNLESS(AgentState.contains(agentId));
    auto& state = AgentState[agentId];
    Y_DEBUG_ABORT_UNLESS(state.SmartResyncActor);

    NCloud::Send(
        ActorContext(),
        PartConfig->GetParentActorId(),
        std::make_unique<TEvVolume::TEvUpdateSmartResyncState>(
            processedBlockCount,
            blockCountNeedToBeProcessed));
}

void TIncompleteMirrorRWModeControllerActor::OnMigrationFinished(
    const TString& agentId)
{
    Y_DEBUG_ABORT_UNLESS(AgentState.contains(agentId));
    auto& state = AgentState[agentId];
    Y_DEBUG_ABORT_UNLESS(state.SmartResyncActor);

    NCloud::Send(
        ActorContext(),
        PartConfig->GetParentActorId(),
        std::make_unique<TEvVolume::TEvSmartResyncFinished>(agentId));
}

void TIncompleteMirrorRWModeControllerActor::OnMigrationError(
    const TString& agentId)
{
    Y_DEBUG_ABORT_UNLESS(AgentState.contains(agentId));
    auto& state = AgentState[agentId];
    Y_DEBUG_ABORT_UNLESS(state.SmartResyncActor);

    // Abort this and start real resync?
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
                StatActorId,
                msg->AgentId));
        auto& state = AgentState[msg->AgentId];
        state.State = EAgentState::Unavailable;
        state.AgentAvailabilityWaiter = waiterActorId;
        state.CleanBlocksMap =
            std::make_shared<TCompressedBitmap>(PartConfig->GetBlockCount());
        state.CleanBlocksMap->Set(0, PartConfig->GetBlockCount());

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
    const NActors::TActorContext& ctx)
{
    const auto* msg = ev->Get();

    Y_DEBUG_ABORT_UNLESS(AgentState.contains(msg->AgentId));
    if (!AgentState.contains(msg->AgentId)) {
        return;
    }

    switch (AgentState[msg->AgentId].State) {
        case EAgentState::Unavailable: {
            TAgentState& state = AgentState[msg->AgentId];
            Y_DEBUG_ABORT_UNLESS(!state.SmartResyncActor);
            Y_DEBUG_ABORT_UNLESS(state.AgentAvailabilityWaiter);

            NCloud::Send<TEvents::TEvPoisonPill>(
                ctx,
                state.AgentAvailabilityWaiter);
            state.AgentAvailabilityWaiter = TActorId();

            state.SmartResyncActor = NCloud::Register(
                ctx,
                std::make_unique<TSmartResyncActor>(
                    this,
                    Config,
                    PartConfig,
                    ProfileLog,
                    BlockDigestGenerator,
                    RwClientId,
                    PartNonreplActorId,
                    StatActorId,
                    MirrorPartitionActor,
                    SelfId(),
                    state.CleanBlocksMap,
                    msg->AgentId));
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
            MarkBlocksAsDirty(deviceRequest.Device.GetAgentId(), deviceRequest.BlockRange);
        }

        // Repond with fake "S_OK".
        using TResponse = TMethod::TResponse;
        auto response = std::make_unique<TResponse>();
        NCloud::Reply(ctx, *ev, std::move(response));
        return;
    }

    using TRequest = TMethod::TRequest;
    auto request = std::make_unique<TRequest>();
    request->Record = std::move(msg->Record);
    request->CallContext = msg->CallContext;
    if (deviceRequests.size() == 2 && availableAgentCount == 1) {
        // In this case we should discard blocks which would be written in the
        // unavailable agent.
        Y_DEBUG_ABORT_UNLESS(
            requestBlockCount == rangeToDelete.Size() + rangeToWrite.Size());
        TrimRequest(ev, rangeToWrite, rangeToDelete, unavailableAgentId);
    }

    // Who will be recipient of ok request: SmartResyncActor or
    // PartNonreplActorId ?
    ctx.Send(std::make_unique<IEventHandle>(
        PartNonreplActorId,
        ev->Sender,
        request.release(),
        ev->Flags,
        ev->Cookie,
        &ev->Sender));
}

template <typename TMethod>
void TIncompleteMirrorRWModeControllerActor::WriteRequest2(
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

    // TODO: check if we can respond right away.

    const bool shouldRespondWithSuccess = AllOf(deviceRequests, [this](const auto& deviceRequest){
        return AgentIsUnavailable(deviceRequest.Device.GetAgentId());
    });
    if (shouldRespondWithSuccess) {
        // TODO: MARK DIRTY BLOCKS
        NCloud::Reply(ctx, *ev, std::make_unique<TMethod::TResponse>());
        return;
    }

    auto requests = SplitRequest(ev, deviceRequests);
    NCloud::Register(
        ctx,
        std::make_unique<TSplitRequestSenderActor<TMethod>>(
            CreateRequestInfo<TMethod>(
                ev->Sender,
                ev->Cookie,
                msg->CallContext),
            std::move(requests),
            msg->Record.GetDiskId(),
            SelfId(),
            GetRequestId(msg->Record)));
}

template <typename TMethod>
TVector<TSplitRequest> TIncompleteMirrorRWModeControllerActor::SplitRequest(
    const TMethod::TRequest::TPtr& ev,
    const TVector<TDeviceRequest>& deviceRequests)
{
    auto* msg = ev->Get();
    if (!ShouldSplitWriteRequest(deviceRequests)) {
        auto request = std::make_unique<TMethod::TRequest>();
        request->Record = std::move(msg->Record);
        request->CallContext = msg->CallContext;

        return {TSplitRequest(
            std::move(request),
            msg->CallContext,
            GetRecipientActorId(deviceRequests[0].Device.GetAgentId()))};
    }

    return DoSplitRequest(ev, deviceRequests);
}

TVector<TSplitRequest> TIncompleteMirrorRWModeControllerActor::DoSplitRequest(
    const TEvService::TEvWriteBlocksRequest::TPtr& ev,
    const TVector<TDeviceRequest>& deviceRequests)
{
    TVector<TSplitRequest> result;
    auto* msg = ev->Get();

    TDeviceRequestBuilder builder(
        deviceRequests,
        PartConfig->GetBlockSize(),
        msg->Record);

    for (const auto& deviceRequest: deviceRequests) {
        auto request = std::make_unique<TEvService::TEvWriteBlocksRequest>();

        auto& callContext = *msg->CallContext;
        if (!callContext.LWOrbit.Fork(request->CallContext->LWOrbit)) {
            LWTRACK(
                ForkFailed,
                callContext.LWOrbit,
                TEvService::TWriteBlocksMethod::Name,
                callContext.RequestId);
        }
        auto forkedCallContext = request->CallContext;

        request->Record.MutableHeaders()->CopyFrom(msg->Record.GetHeaders());
        request->Record.SetDiskId(msg->Record.GetDiskId());
        request->Record.SetStartIndex(deviceRequest.BlockRange.Start);
        request->Record.SetFlags(msg->Record.GetFlags());
        request->Record.SetSessionId(msg->Record.GetSessionId());

        builder.BuildNextRequest(request->Record);

        result.emplace_back(
            std::move(request),
            forkedCallContext,
            GetRecipientActorId(deviceRequest.Device.GetAgentId()));
    }

    return result;
}

TVector<TSplitRequest> TIncompleteMirrorRWModeControllerActor::DoSplitRequest(
    const TEvService::TEvWriteBlocksLocalRequest::TPtr& ev,
    const TVector<TDeviceRequest>& deviceRequests)
{
    TVector<TSplitRequest> result;
    auto* msg = ev->Get();

    TDeviceRequestBuilder builder(
        deviceRequests,
        PartConfig->GetBlockSize(),
        msg->Record);

    for (const auto& deviceRequest: deviceRequests) {
        auto request =
            std::make_unique<TEvService::TEvWriteBlocksLocalRequest>();

        auto& callContext = *msg->CallContext;
        if (!callContext.LWOrbit.Fork(request->CallContext->LWOrbit)) {
            LWTRACK(
                ForkFailed,
                callContext.LWOrbit,
                TEvService::TWriteBlocksMethod::Name,
                callContext.RequestId);
        }
        auto forkedCallContext = request->CallContext;
        request->Record.MutableHeaders()->CopyFrom(msg->Record.GetHeaders());
        request->Record.SetDiskId(msg->Record.GetDiskId());
        request->Record.SetStartIndex(deviceRequest.BlockRange.Start);
        request->Record.SetFlags(msg->Record.GetFlags());
        request->Record.SetSessionId(msg->Record.GetSessionId());

        request->Record.BlocksCount = deviceRequest.BlockRange.Size();
        request->Record.BlockSize = msg->Record.BlockSize;

        TSgList sglist;
        builder.BuildNextRequest(&sglist);
        Y_DEBUG_ABORT_UNLESS(!sglist.empty());
        request->Record.Sglist =
            msg->Record.Sglist.CreateDepender(std::move(sglist));

        result.emplace_back(
            std::move(request),
            forkedCallContext,
            GetRecipientActorId(deviceRequest.Device.GetAgentId()));
    }

    return result;
}

TVector<TSplitRequest> TIncompleteMirrorRWModeControllerActor::DoSplitRequest(
    const TEvService::TEvZeroBlocksRequest::TPtr& ev,
    const TVector<TDeviceRequest>& deviceRequests)
{
    TVector<TSplitRequest> result;
    auto* msg = ev->Get();

    for (const auto& deviceRequest: deviceRequests) {
        auto request = std::make_unique<TEvService::TEvZeroBlocksRequest>();

        auto& callContext = *msg->CallContext;
        if (!callContext.LWOrbit.Fork(request->CallContext->LWOrbit)) {
            LWTRACK(
                ForkFailed,
                callContext.LWOrbit,
                TEvService::TWriteBlocksMethod::Name,
                callContext.RequestId);
        }
        auto forkedCallContext = request->CallContext;

        request->Record.MutableHeaders()->CopyFrom(msg->Record.GetHeaders());
        request->Record.SetDiskId(msg->Record.GetDiskId());
        request->Record.SetStartIndex(deviceRequest.BlockRange.Start);
        request->Record.SetFlags(msg->Record.GetFlags());
        request->Record.SetSessionId(msg->Record.GetSessionId());
        request->Record.SetBlocksCount(deviceRequest.BlockRange.Size());

        builder.BuildNextRequest(request->Record);

        result.emplace_back(
            std::move(request),
            forkedCallContext,
            GetRecipientActorId(deviceRequest.Device.GetAgentId()));
    }

    return result;
}

void TIncompleteMirrorRWModeControllerActor::SendPrepaerdRequest(
    const NActors::TActorContext& ctx,
    const TEvService::TEvWriteBlocksRequest::TPtr& originalEvent,
    std::unique_ptr<TEvService::TEvWriteBlocksRequest> request,
    const TString& agentId)
{
    NActors::TActorId recipient = GetRecipientActorId(agentId);
    NActors::TActorId nondeliveryActor =
        originalEvent->GetForwardOnNondeliveryRecipient();
    ctx.Send(std::make_unique<IEventHandle>(
        PartNonreplActorId,
        originalEvent->Sender,
        request.release(),
        originalEvent->Flags,
        originalEvent->Cookie,
        originalEvent->Flags & NActors::IEventHandle::FlagForwardOnNondelivery
            ? &nondeliveryActor
            : nullptr));
}

NActors::TActorId TIncompleteMirrorRWModeControllerActor::GetRecipientActorId(
    const TString& agentId) const
{
    if (!AgentState.contains(agentId)) {
        return PartNonreplActorId;
    }

    switch(AgentState.at(agentId).State) {
        case EAgentState::Unavailable:
            return {};
        case EAgentState::Resyncing:
            Y_DEBUG_ABORT_UNLESS(AgentState.at(agentId).SmartResyncActor);
            return AgentState.at(agentId).SmartResyncActor;
    }
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

void TIncompleteMirrorRWModeControllerActor::HandleRWClientIdChanged(
    const TEvVolume::TEvRWClientIdChanged::TPtr& ev,
    const TActorContext& ctx)
{
    RwClientId = ev->Get()->RWClientId;
    auto& callContext = *ev->Get()->CallContext;

    {
        auto partitionRequest =
            std::make_unique<TEvVolume::TEvRWClientIdChanged>(RwClientId);
        if (!callContext.LWOrbit.Fork(partitionRequest->CallContext->LWOrbit)) {
            LWTRACK(
                ForkFailed,
                callContext.LWOrbit,
                "TEvVolume::TEvRWClientIdChanged",
                callContext.RequestId);
        }
        ctx.Send(PartNonreplActorId, std::move(partitionRequest));
    }

    for (const auto& state: AgentState) {
        Y_DEBUG_ABORT_UNLESS(
            !state.second.AgentAvailabilityWaiter ||
            !state.second.SmartResyncActor);

        auto companionRequest =
            std::make_unique<TEvVolume::TEvRWClientIdChanged>(RwClientId);
        if (!callContext.LWOrbit.Fork(companionRequest->CallContext->LWOrbit)) {
            LWTRACK(
                ForkFailed,
                callContext.LWOrbit,
                "TEvVolume::TEvRWClientIdChanged",
                callContext.RequestId);
        }

        if (state.second.AgentAvailabilityWaiter) {
            ctx.Send(
                state.second.AgentAvailabilityWaiter,
                std::move(companionRequest));
        } else if (state.second.SmartResyncActor) {
            ctx.Send(
                state.second.SmartResyncActor,
                std::move(companionRequest));
        }
    }
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

    Become(&TThis::StateZombie);
    NCloud::Send<TEvents::TEvPoisonPill>(ctx, PartNonreplActorId);
    for (const auto& state: AgentState) {
        if (state.second.AgentAvailabilityWaiter) {
            NCloud::Send<TEvents::TEvPoisonPill>(
                ctx,
                state.second.AgentAvailabilityWaiter);
        }
        if (state.second.SmartResyncActor) {
            NCloud::Send<TEvents::TEvPoisonPill>(
                ctx,
                state.second.SmartResyncActor);
        }
    }
    AgentState.clear();
}

void TIncompleteMirrorRWModeControllerActor::HandlePoisonTaken(
    const TEvents::TEvPoisonTaken::TPtr& ev,
    const TActorContext& ctx)
{
    if (ev->Sender != PartNonreplActorId) {
        return;
    }

    NCloud::Send<TEvents::TEvPoisonTaken>(ctx, MirrorPartitionActor);
    Die(ctx);
}

////////////////////////////////////////////////////////////////////////////////

STFUNC(TIncompleteMirrorRWModeControllerActor::StateWork)
{
    switch (ev->GetTypeRewrite()) {
        HFunc(TEvService::TEvWriteBlocksRequest, HandleWriteBlocks);
        HFunc(TEvService::TEvWriteBlocksLocalRequest, HandleWriteBlocksLocal);
        HFunc(TEvService::TEvZeroBlocksRequest, HandleZeroBlocks);

        HFunc(
            NPartition::TEvPartition::TEvAgentIsUnavailable,
            HandleAgentIsUnavailable);
        HFunc(
            NPartition::TEvPartition::TEvAgentIsBackOnline,
            HandleAgentIsBackOnline);
        HFunc(TEvVolume::TEvRWClientIdChanged, HandleRWClientIdChanged);
        // HFunc(NPartition::TEvPartition::TEvDrainRequest, HandleDrain);

        // HFunc(TEvService::TEvReadBlocksLocalRequest, HandleReadBlocksLocal);

        // HFunc(TEvPartition::TEvDrainRequest,
        // DrainActorCompanion.HandleDrain);
        // HFunc(TEvPartition::TEvEnterIncompleteMirrorRWModeRequest,
        // HandleEnterIncompleteMirrorRWMode);

        // HFunc(
        //     TEvService::TEvGetChangedBlocksRequest,
        //     GetChangedBlocksCompanion.HandleGetChangedBlocks);

        // HFunc(TEvVolume::TEvDescribeBlocksRequest, HandleDescribeBlocks);
        // HFunc(TEvVolume::TEvGetCompactionStatusRequest,
        // HandleGetCompactionStatus); HFunc(TEvVolume::TEvCompactRangeRequest,
        // HandleCompactRange); HFunc(TEvVolume::TEvRebuildMetadataRequest,
        // HandleRebuildMetadata);
        // HFunc(TEvVolume::TEvGetRebuildMetadataStatusRequest,
        // HandleGetRebuildMetadataStatus); HFunc(TEvVolume::TEvScanDiskRequest,
        // HandleScanDisk); HFunc(TEvVolume::TEvGetScanDiskStatusRequest,
        // HandleGetScanDiskStatus);

        // HFunc(
        //     TEvNonreplPartitionPrivate::TEvWriteOrZeroCompleted,
        //     HandleWriteOrZeroCompleted);

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

        HFunc(TEvService::TEvWriteBlocksRequest, RejectWriteBlocks);
        HFunc(TEvService::TEvZeroBlocksRequest, RejectZeroBlocks);
        HFunc(TEvService::TEvWriteBlocksLocalRequest, RejectWriteBlocksLocal);

        // HFunc(NPartition::TEvPartition::TEvDrainRequest, RejectDrain);

        // HFunc(TEvVolume::TEvDescribeBlocksRequest, RejectDescribeBlocks);
        // HFunc(TEvVolume::TEvGetCompactionStatusRequest,
        // RejectGetCompactionStatus); HFunc(TEvVolume::TEvCompactRangeRequest,
        // RejectCompactRange); HFunc(TEvVolume::TEvRebuildMetadataRequest,
        // RejectRebuildMetadata);
        // HFunc(TEvVolume::TEvGetRebuildMetadataStatusRequest,
        // RejectGetRebuildMetadataStatus); HFunc(TEvVolume::TEvScanDiskRequest,
        // RejectScanDisk); HFunc(TEvVolume::TEvGetScanDiskStatusRequest,
        // RejectGetScanDiskStatus);

        // IgnoreFunc(TEvNonreplPartitionPrivate::TEvWriteOrZeroCompleted);

        // IgnoreFunc(TEvVolume::TEvDiskRegistryBasedPartitionCounters);

        IgnoreFunc(TEvVolume::TEvRWClientIdChanged);
        IgnoreFunc(TEvents::TEvPoisonPill);
        HFunc(TEvents::TEvPoisonTaken, HandlePoisonTaken);

        default:
            ForwardUnexpectedEvent(ev, ActorContext());
            break;
    }
}

}   // namespace NCloud::NBlockStore::NStorage
