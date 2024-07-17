#pragma once

#include <cloud/blockstore/libs/storage/core/public.h>
#include <cloud/storage/core/libs/common/error.h>

#include <util/datetime/base.h>
#include <util/generic/fwd.h>

#include <memory>

namespace NCloud::NBlockStore::NProto {

class TAgentConfig;
class TDeviceConfig;

}   // namespace NCloud::NBlockStore::NProto

namespace NCloud::NBlockStore::NStorage::NDiskRegistry {

////////////////////////////////////////////////////////////////////////////////

struct IDiskRegistry
{
    virtual ~IDiskRegistry() = default;

    [[nodiscard]] virtual bool HasDependentSsdDisks(
        const NProto::TAgentConfig& agent) const = 0;

    [[nodiscard]] virtual bool HasDependentSsdDisks(
        const NProto::TDeviceConfig& device) const = 0;

    [[nodiscard]] virtual ui32
    CountBrokenHddPlacementGroupPartitionsAfterAgentRemoval(
        const NProto::TAgentConfig& agent) const = 0;

    [[nodiscard]] virtual ui32
    CountBrokenHddPlacementGroupPartitionsAfterDeviceRemoval(
        const NProto::TDeviceConfig& device) const = 0;
};

////////////////////////////////////////////////////////////////////////////////

struct ICms
{
    virtual ~ICms() = default;

    struct TResult
    {
        NProto::TError Error;

        TInstant CmsTs;
        TDuration Timeout;
    };

    virtual TResult AddHost(
        IDiskRegistry& registry,
        const NProto::TAgentConfig& agent,
        TInstant now) = 0;

    virtual TResult RemoveHost(
        IDiskRegistry& registry,
        const NProto::TAgentConfig& agent,
        TInstant now) = 0;

    virtual TResult AddDevice(
        IDiskRegistry& registry,
        const NProto::TAgentConfig& agent,
        const NProto::TDeviceConfig& device,
        TInstant now) = 0;

    virtual TResult RemoveDevice(
        IDiskRegistry& registry,
        const NProto::TAgentConfig& agent,
        const NProto::TDeviceConfig& device,
        TInstant now) = 0;

    [[nodiscard]] virtual bool IsCmsRequestActive(
        const NProto::TAgentConfig& agent,
        TInstant now) const = 0;
};

////////////////////////////////////////////////////////////////////////////////

std::shared_ptr<ICms> CreateCMS(TStorageConfigPtr config);

}   // namespace NCloud::NBlockStore::NStorage::NDiskRegistry
