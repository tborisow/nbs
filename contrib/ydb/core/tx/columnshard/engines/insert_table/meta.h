#pragma once
#include <contrib/ydb/core/formats/arrow/special_keys.h>
#include <contrib/ydb/core/tx/columnshard/blob.h>
#include <contrib/ydb/core/tx/columnshard/engines/defs.h>
#include <contrib/ydb/core/protos/tx_columnshard.pb.h>
#include <contrib/ydb/library/accessor/accessor.h>

namespace NKikimr::NOlap {

class TInsertedDataMeta {
private:
    YDB_READONLY_DEF(TInstant, DirtyWriteTime);
    YDB_READONLY(ui32, NumRows, 0);
    YDB_READONLY(ui64, RawBytes, 0);

    mutable bool KeysParsed = false;
    mutable std::optional<NArrow::TFirstLastSpecialKeys> SpecialKeysParsed;

    NKikimrTxColumnShard::TLogicalMetadata OriginalProto;

    const std::optional<NArrow::TFirstLastSpecialKeys>& GetSpecialKeys() const;
public:
    TInsertedDataMeta(const NKikimrTxColumnShard::TLogicalMetadata& proto)
        : OriginalProto(proto)
    {
        AFL_VERIFY(proto.HasDirtyWriteTimeSeconds())("data", proto.DebugString());
        DirtyWriteTime = TInstant::Seconds(proto.GetDirtyWriteTimeSeconds());
        NumRows = proto.GetNumRows();
        RawBytes = proto.GetRawBytes();
    }

    std::optional<NArrow::TReplaceKey> GetFirstPK(const std::shared_ptr<arrow::Schema>& schema) const {
        if (GetSpecialKeys()) {
            return GetSpecialKeys()->GetFirst(schema);
        } else {
            return {};
        }
    }
    std::optional<NArrow::TReplaceKey> GetLastPK(const std::shared_ptr<arrow::Schema>& schema) const {
        if (GetSpecialKeys()) {
            return GetSpecialKeys()->GetLast(schema);
        } else {
            return {};
        }
    }

    NKikimrTxColumnShard::TLogicalMetadata SerializeToProto() const;

};

}
