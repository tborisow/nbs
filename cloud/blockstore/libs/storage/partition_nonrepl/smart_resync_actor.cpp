#include "smart_resync_actor.h"

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
    IProfileLogPtr profileLog,
    IBlockDigestGeneratorPtr blockDigestGenerator,
    TString rwClientId,
    NActors::TActorId partNonreplActorId,
    NActors::TActorId statActorId,
    NActors::TActorId mirrorPartitionActor,
    std::shared_ptr<TCompressedBitmap> migrationBlockMap)
    : TNonreplicatedPartitionMigrationCommonActor(
          this,
          config,
          partConfig->GetName(),
          partConfig->GetBlockCount(),
          partConfig->GetBlockSize(),
          profileLog,
          blockDigestGenerator,
          std::move(migrationBlockMap),
          std::move(rwClientId),
          statActorId,
          config->GetMaxMigrationIoDepth())
    , Config(std::move(config))
    , PartConfig(std::move(partConfig))
    , ProfileLog(std::move(profileLog))
    , BlockDigestGenerator(std::move(blockDigestGenerator))
    , PartNonreplActorId(partNonreplActorId)
    , StatActorId(statActorId)
    , MirrorPartitionActor(mirrorPartitionActor)
{}

TSmartResyncActor::~TSmartResyncActor() = default;

void TSmartResyncActor::OnBootstrap(const TActorContext& ctx)
{
    Y_UNUSED(ctx);

}

void TSmartResyncActor::HandlePoisonPill(
    const NActors::TEvents::TEvPoisonPill::TPtr& ev,
    const NActors::TActorContext& ctx)
{
    Y_UNUSED(ev);
    Die(ctx);
}

bool TSmartResyncActor::OnMessage(
    const NActors::TActorContext& ctx,
    TAutoPtr<NActors::IEventHandle>& ev)
{
    switch (ev->GetTypeRewrite()) {
        // HFunc(
        //     TEvVolumePrivate::TEvShadowDiskAcquired,
        //     HandleShadowDiskAcquired);


        // Write/zero request.
        // case TEvService::TEvWriteBlocksRequest::EventType: {
        //     return HandleWriteZeroBlocks<TEvService::TWriteBlocksMethod>(
        //         *reinterpret_cast<TEvService::TEvWriteBlocksRequest::TPtr*>(
        //             &ev),
        //         ctx);
        // }
        // case TEvService::TEvWriteBlocksLocalRequest::EventType: {
        //     return HandleWriteZeroBlocks<TEvService::TWriteBlocksLocalMethod>(
        //         *reinterpret_cast<
        //             TEvService::TEvWriteBlocksLocalRequest::TPtr*>(&ev),
        //         ctx);
        // }
        // case TEvService::TEvZeroBlocksRequest::EventType: {
        //     return HandleWriteZeroBlocks<TEvService::TZeroBlocksMethod>(
        //         *reinterpret_cast<TEvService::TEvZeroBlocksRequest::TPtr*>(&ev),
        //         ctx);
        // }

        default:
            // Message processing by the base class is required.
            return false;
    }

    // We get here if we have processed an incoming message. And its processing
    // by the base class is not required.
    return true;
}

TDuration TSmartResyncActor::CalculateMigrationTimeout(TBlockRange64 range)
{}

void TSmartResyncActor::OnMigrationProgress(
    const NActors::TActorContext& ctx,
    ui64 migrationIndex)
{}

void TSmartResyncActor::OnMigrationFinished(const NActors::TActorContext& ctx)
{}

void TSmartResyncActor::OnMigrationError(const NActors::TActorContext& ctx)
{}

}   // namespace NCloud::NBlockStore::NStorage
