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

////////////////////////////////////////////////////////////////////////////////

class ISmartResyncDelegate
{
public:
    virtual ~ISmartResyncDelegate() = default;

    // Notifies that a sufficiently large sequence of data has been migrated.
    // The size is determined by the settings.
    virtual void OnMigrationProgress(
        const TString& agentId,
        ui64 processedBlockCount,
        ui64 blockCountNeedToBeProcessed) = 0;

    // Notifies that the data migration was completed successfully.
    virtual void OnMigrationFinished(const TString& agentId) = 0;

    // Notifies that a non-retriable error occurred during the migration and it
    // was stopped.
    virtual void OnMigrationError(const TString& agentId) = 0;
};

////////////////////////////////////////////////////////////////////////////////

class TSmartResyncActor final
    : public TNonreplicatedPartitionMigrationCommonActor,
        public IMigrationOwner
{
private:
    ISmartResyncDelegate* const Delegate;
    const TStorageConfigPtr Config;
    const TNonreplicatedPartitionConfigPtr PartConfig;
    const IProfileLogPtr ProfileLog;
    const IBlockDigestGeneratorPtr BlockDigestGenerator;
    const NActors::TActorId PartNonreplActorId;
    // const NActors::TActorId StatActorId;
    const NActors::TActorId MirrorPartitionActor;
    const NActors::TActorId ParentActor;
    const TString AgentId;

    std::shared_ptr<TCompressedBitmap> BlockMap;

public:
    TSmartResyncActor(
        ISmartResyncDelegate* delegate,
        TStorageConfigPtr config,
        TNonreplicatedPartitionConfigPtr partConfig,
        IProfileLogPtr profileLog,
        IBlockDigestGeneratorPtr blockDigestGenerator,
        TString rwClientId,
        NActors::TActorId partNonreplActorId,
        NActors::TActorId statActorId,
        NActors::TActorId mirrorPartitionActor,
        NActors::TActorId parentActor,
        std::shared_ptr<TCompressedBitmap> migrationBlockMap,
        TString agentId);

    ~TSmartResyncActor() override;

private:
    // IMigrationOwner implementation
    void OnBootstrap(const NActors::TActorContext& ctx) override;
    bool OnMessage(
        const NActors::TActorContext& ctx,
        TAutoPtr<NActors::IEventHandle>& ev) override;
     void OnMigrationProgress(
        const NActors::TActorContext& ctx,
        ui64 migrationIndex) override;
     void OnMigrationFinished(const NActors::TActorContext& ctx) override;
     void OnMigrationError(const NActors::TActorContext& ctx) override;

private:

};

}   // namespace NCloud::NBlockStore::NStorage
