#include "abstract.h"
#include <contrib/ydb/core/formats/arrow/serializer/abstract.h>
#include <contrib/ydb/core/formats/arrow/dictionary/diff.h>

namespace NKikimr::NKqp::NColumnshard {

class TAlterColumnOperation : public ITableStoreOperation {
private:
    static TString GetTypeName() {
        return "ALTER_COLUMN";
    }

    static inline auto Registrator = TFactory::TRegistrator<TAlterColumnOperation>(GetTypeName());

    TString ColumnName;

    NArrow::NSerialization::TSerializerContainer Serializer;
    NArrow::NDictionary::TEncodingDiff DictionaryEncodingDiff;
public:
    TConclusionStatus DoDeserialize(NYql::TObjectSettingsImpl::TFeaturesExtractor& features) override;

    void DoSerializeScheme(NKikimrSchemeOp::TAlterColumnTableSchema& schemaData) const override;
};

}

