#include "disk_registry_cms.h"

#include <cloud/blockstore/libs/storage/core/config.h>
#include <cloud/blockstore/libs/storage/protos/disk.pb.h>

#include <util/string/builder.h>

namespace NCloud::NBlockStore::NStorage::NDiskRegistry {

namespace {

////////////////////////////////////////////////////////////////////////////////

constexpr TDuration CMS_UPDATE_STATE_TO_ONLINE_TIMEOUT = TDuration::Minutes(5);

////////////////////////////////////////////////////////////////////////////////

TDuration GetInfraTimeout(
    const TStorageConfig& config,
    NProto::EAgentState agentState)
{
    if (agentState == NProto::AGENT_STATE_UNAVAILABLE) {
        return config.GetNonReplicatedInfraUnavailableAgentTimeout();
    }

    return config.GetNonReplicatedInfraTimeout();
}

////////////////////////////////////////////////////////////////////////////////

class TCms final:
    public ICms
{
private:
    const TStorageConfigPtr Config;

public:
    explicit TCms(TStorageConfigPtr config);

    TResult AddHost(
        IDiskRegistry& registry,
        const NProto::TAgentConfig& agent,
        TInstant now) override;

    TResult RemoveHost(
        IDiskRegistry& registry,
        const NProto::TAgentConfig& agent,
        TInstant now) override;

    TResult AddDevice(
        IDiskRegistry& registry,
        const NProto::TAgentConfig& agent,
        const NProto::TDeviceConfig& device,
        TInstant now) override;

    TResult RemoveDevice(
        IDiskRegistry& registry,
        const NProto::TAgentConfig& agent,
        const NProto::TDeviceConfig& device,
        TInstant now) override;

    [[nodiscard]] bool IsCmsRequestActive(
        const NProto::TAgentConfig& agent,
        TInstant now) const override;
};

////////////////////////////////////////////////////////////////////////////////

TCms::TCms(TStorageConfigPtr config)
    : Config(std::move(config))
{}

auto TCms::AddHost(
    IDiskRegistry& registry,
    const NProto::TAgentConfig& agent,
    TInstant now) -> TResult
{
    Y_UNUSED(registry);

    TResult result{
        .Error = {},
        .CmsTs = TInstant::MicroSeconds(agent.GetCmsTs()),
        .Timeout = {},
    };

    if (!result.CmsTs) {
        result.CmsTs = now;
    }

    // Agent can return from 'unavailable' state only when it is reconnected to
    // the cluster.
    if (agent.GetState() == NProto::AGENT_STATE_UNAVAILABLE) {
        result.Timeout = result.CmsTs + CMS_UPDATE_STATE_TO_ONLINE_TIMEOUT - now;
        if (!result.Timeout) {
            result.Error.SetCode(E_INVALID_STATE);
        }
    }

    if (result.Timeout) {
        result.Error = MakeError(
            E_TRY_AGAIN,
            TStringBuilder() << "time remaining: " << result.Timeout);
    } else {
        result.CmsTs = {};
    }

    return result;
}

auto TCms::RemoveHost(
    IDiskRegistry& registry,
    const NProto::TAgentConfig& agent,
    TInstant now) -> TResult
{
    TResult result{
        .Error = {},
        .CmsTs = TInstant::MicroSeconds(agent.GetCmsTs()),
        .Timeout = {},
    };

    if (result.CmsTs == TInstant::Zero()) {
        result.CmsTs = now;
    }

    const auto infraTimeout = GetInfraTimeout(*Config, agent.GetState());

    if (result.CmsTs + infraTimeout <= now
            && agent.GetState() < NProto::AGENT_STATE_UNAVAILABLE)
    {
        // restart timer
        result.CmsTs = now;
    }

    result.Timeout = result.CmsTs + infraTimeout - now;

    const bool hasDependentDisks = registry.HasDependentSsdDisks(agent);
    const ui32 brokenPlacementGroupPartitions =
        registry.CountBrokenHddPlacementGroupPartitionsAfterAgentRemoval(agent);
    const ui32 maxBrokenHddPartitions =
        Config->GetMaxBrokenHddPlacementGroupPartitionsAfterDeviceRemoval();
    if (!hasDependentDisks
            && brokenPlacementGroupPartitions <= maxBrokenHddPartitions)
    {
        // no dependent disks => we can return this host immediately
        result.Timeout = TDuration::Zero();
    }

    if (result.Timeout) {
        result.Error = MakeError(
            E_TRY_AGAIN,
            TStringBuilder() << "time remaining: " << result.Timeout);
    } else {
        result.CmsTs = TInstant::Zero();
    }

    return result;
}

auto TCms::AddDevice(
    IDiskRegistry& registry,
    const NProto::TAgentConfig& agent,
    const NProto::TDeviceConfig& device,
    TInstant now) -> TResult
{
    Y_UNUSED(registry);
    Y_UNUSED(agent);
    Y_UNUSED(now);

    if (device.GetState() == NProto::DEVICE_STATE_ERROR) {
        // CMS can't return device from 'error' state.
        return {
            .Error = MakeError(E_INVALID_STATE, "device is in error state")};
    }

    return {};
}

auto TCms::RemoveDevice(
    IDiskRegistry& registry,
    const NProto::TAgentConfig& agent,
    const NProto::TDeviceConfig& device,
    TInstant now) -> TResult
{
    Y_UNUSED(agent);

    TInstant cmsTs;
    NProto::TError error;
    TDuration timeout;

    const bool hasDependentDisk = registry.HasDependentSsdDisks(device)
        && device.GetState() < NProto::DEVICE_STATE_ERROR;

    if (hasDependentDisk) {
        error = MakeError(E_TRY_AGAIN, "have dependent disk");
    }

    const ui32 brokenPlacementGroupPartitions =
        registry.CountBrokenHddPlacementGroupPartitionsAfterDeviceRemoval(device);
    const ui32 maxBrokenHddPartitions =
        Config->GetMaxBrokenHddPlacementGroupPartitionsAfterDeviceRemoval();

    if (brokenPlacementGroupPartitions > maxBrokenHddPartitions) {
        error = MakeError(
            E_TRY_AGAIN,
            TStringBuilder() << "will break too many partitions: "
                << brokenPlacementGroupPartitions);
    }

    if (HasError(error)) {
        cmsTs = TInstant::MicroSeconds(device.GetCmsTs());
        if (!cmsTs || cmsTs + Config->GetNonReplicatedInfraTimeout() <= now) {
            // restart timer
            cmsTs = now;
        }

        timeout = cmsTs + Config->GetNonReplicatedInfraTimeout() - now;
    }

    return {
        .Error = std::move(error),
        .CmsTs = cmsTs,
        .Timeout = timeout
    };
}

bool TCms::IsCmsRequestActive(
    const NProto::TAgentConfig& agent,
    TInstant now) const
{
    const auto cmsTs = TInstant::MicroSeconds(agent.GetCmsTs());
    const auto cmsDeadline = cmsTs + GetInfraTimeout(*Config, agent.GetState());

    return cmsTs && cmsDeadline > now;
}

}   // namespace

////////////////////////////////////////////////////////////////////////////////

std::shared_ptr<ICms> CreateCMS(TStorageConfigPtr config)
{
    return std::make_shared<TCms>(std::move(config));
}

}   // namespace NCloud::NBlockStore::NStorage::NDiskRegistry
