# Vendored zstd 1.5.6, decompress path only: Pia compresses ~87% of the datagrams the
# Switch sends. The compress side needs no encoder -- PiaCrypto rewrites the frame header
# to a canonical 28b52ffd0018 anyway, so raw-block frames are emitted directly.
# Upstream sources are unmodified; re-vendor rather than edit.
idf_component_register(
    SRCS "lib/common/debug.c" "lib/common/entropy_common.c" "lib/common/error_private.c"
         "lib/common/fse_decompress.c" "lib/common/xxhash.c" "lib/common/zstd_common.c"
         "lib/decompress/huf_decompress.c" "lib/decompress/zstd_ddict.c"
         "lib/decompress/zstd_decompress.c" "lib/decompress/zstd_decompress_block.c"
    INCLUDE_DIRS "lib"
    PRIV_INCLUDE_DIRS "lib/common" "lib/decompress")
# No x86 assembly on Xtensa, and no threading in the decoder.
# The decoder's literal overflow buffer defaults to 64 KB, which alone puts a DCtx at
# ~94 KB -- more than the largest free block once Wi-Fi is up. Pia's bodies are small
# enough that their literals live in the destination, so this buffer is barely touched
# (CONFIG_PIA_ZSTD_LITERAL_BUFFER). ZSTD_DECODER_INTERNAL_BUFFER is guarded by #ifndef.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ZSTD_DISABLE_ASM=1 ZSTD_MULTITHREAD=0 ZSTD_DECODER_INTERNAL_BUFFER=${CONFIG_PIA_ZSTD_LITERAL_BUFFER})
