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
#include <cloud/blockstore/libs/storage/partition_nonrepl/part_nonrepl_migration_common_actor.h>
#include <cloud/storage/core/libs/common/compressed_bitmap.h>

#include <contrib/ydb/library/actors/core/actor_bootstrapped.h>
#include <contrib/ydb/library/actors/core/events.h>
#include <contrib/ydb/library/actors/core/hfunc.h>
#include <contrib/ydb/library/actors/core/mon.h>

namespace NCloud::NBlockStore::NStorage {

class TSmartResyncActor final
    : public TNonreplicatedPartitionMigrationCommonActor,
        public IMigrationOwner
{
private:
    const TStorageConfigPtr Config;
    const TNonreplicatedPartitionConfigPtr PartConfig;
    const IProfileLogPtr ProfileLog;
    const IBlockDigestGeneratorPtr BlockDigestGenerator;
    // const TString RwClientId;
    const NActors::TActorId PartNonreplActorId;
    const NActors::TActorId StatActorId;
    const NActors::TActorId MirrorPartitionActor;

    std::shared_ptr<TCompressedBitmap> BlockMap;

public:
    // TNonreplicatedPartitionMigrationCommonActor(
    //     IMigrationOwner* migrationOwner,
    //     TStorageConfigPtr config,
    //     TString diskId,
    //     ui64 blockCount,
    //     ui64 blockSize,
    //     IProfileLogPtr profileLog,
    //     IBlockDigestGeneratorPtr digestGenerator,
    //     TCompressedBitmap migrationBlockMap,
    //     TString rwClientId,
    //     NActors::TActorId statActorId,
    //     ui32 maxIoDepth);

    TSmartResyncActor(
        TStorageConfigPtr config,
        TNonreplicatedPartitionConfigPtr partConfig,
        IProfileLogPtr profileLog,
        IBlockDigestGeneratorPtr blockDigestGenerator,
        TString rwClientId,
        NActors::TActorId partNonreplActorId,
        NActors::TActorId statActorId,
        NActors::TActorId mirrorPartitionActor,
        std::shared_ptr<TCompressedBitmap> migrationBlockMap);

    ~TSmartResyncActor() override;

private:
    // IMigrationOwner implementation
    void OnBootstrap(const NActors::TActorContext& ctx) override;
    bool OnMessage(
        const NActors::TActorContext& ctx,
        TAutoPtr<NActors::IEventHandle>& ev) override;
    [[nodiscard]]  TDuration CalculateMigrationTimeout(TBlockRange64 range) override;
     void OnMigrationProgress(
        const NActors::TActorContext& ctx,
        ui64 migrationIndex) override;
     void OnMigrationFinished(const NActors::TActorContext& ctx) override;
     void OnMigrationError(const NActors::TActorContext& ctx) override;

private:
    void HandlePoisonPill(
        const NActors::TEvents::TEvPoisonPill::TPtr& ev,
        const NActors::TActorContext& ctx);

    void HandlePoisonTaken(
        const NActors::TEvents::TEvPoisonTaken::TPtr& ev,
        const NActors::TActorContext& ctx);
};

}   // namespace NCloud::NBlockStore::NStorage
