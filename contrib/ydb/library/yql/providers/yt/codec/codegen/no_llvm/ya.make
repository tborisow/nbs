LIBRARY()

ADDINCL(
    contrib/ydb/library/yql/providers/yt/codec/codegen
)

SRCDIR(
    contrib/ydb/library/yql/providers/yt/codec/codegen
)

SRCS(
    yt_codec_cg_dummy.cpp
)

PEERDIR(
)

PROVIDES(YT_CODEC_CODEGEN)

YQL_LAST_ABI_VERSION()

END()
