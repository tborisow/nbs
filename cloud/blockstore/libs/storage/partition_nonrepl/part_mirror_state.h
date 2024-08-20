#pragma once

#include "public.h"

#include "config.h"
#include "replica_info.h"

#include <cloud/blockstore/libs/storage/core/public.h>

#include <contrib/ydb/library/actors/core/actorid.h>

#include <util/generic/vector.h>

namespace NCloud::NBlockStore::NStorage {

////////////////////////////////////////////////////////////////////////////////

class TMirrorPartitionState
{
private:
    const TStorageConfigPtr Config;
    const TNonreplicatedPartitionConfigPtr PartConfig;
    TString RWClientId;

    TMigrations Migrations;
    TVector<TReplicaInfo> ReplicaInfos;
    TVector<NActors::TActorId> ReplicaActors;
    TVector<NActors::TActorId> ReplicaActorProxies;

    ui32 ReadReplicaIndex = 0;

    bool MigrationConfigPrepared = false;

public:
    TMirrorPartitionState(
        TStorageConfigPtr config,
        TString rwClientId,
        TNonreplicatedPartitionConfigPtr partConfig,
        TMigrations migrations,
        TVector<TDevices> replicas);

public:
    const TVector<TReplicaInfo>& GetReplicaInfos() const
    {
        return ReplicaInfos;
    }

    void AddReplicaActor(const NActors::TActorId& actorId)
    {
        ReplicaActorProxies.push_back(actorId);
        ReplicaActors.push_back(actorId);
    }

    const TVector<NActors::TActorId>& GetReplicaActors() const
    {
        return ReplicaActorProxies;
    }

    const TVector<NActors::TActorId>& GetRealReplicaActors() const
    {
        return ReplicaActors;
    }

    void SetRWClientId(TString rwClientId)
    {
        RWClientId = std::move(rwClientId);
    }

    const TString& GetRWClientId() const
    {
        return RWClientId;
    }

    [[nodiscard]] NProto::TError SetReplicaProxy(
        ui32 replicaIndex,
        const NActors::TActorId& actorId);
    [[nodiscard]] NProto::TError ResetReplicaProxy(ui32 replicaIndex);
    bool IsProxySet(ui32 replicaIndex) const;

    [[nodiscard]] NProto::TError Validate();
    void PrepareMigrationConfig();
    [[nodiscard]] bool PrepareMigrationConfigForWarningDevices();
    [[nodiscard]] bool PrepareMigrationConfigForFreshDevices();

    [[nodiscard]] NProto::TError NextReadReplica(
        const TBlockRange64 readRange,
        NActors::TActorId* actorId);

    ui32 GetBlockSize() const;

    ui64 GetBlockCount() const;
};

}   // namespace NCloud::NBlockStore::NStorage
