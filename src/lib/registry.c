#include "lib/minimem.h"
#include "lib/compressors/same_page.h"
#include "lib/compressors/bdi.h"
#include "lib/compressors/wkdm.h"
#include "lib/compressors/wkdm64.h"
#include "lib/compressors/lz4_wrap.h"
#include "lib/compressors/lzsse8.h"
#include "lib/compressors/zstd_dict.h"
#include "lib/compressors/delta.h"
#include "lib/compressors/wkdm64.h"
#include "lib/compressors/block_class.h"

static const struct minimem_compressor *compressors[MINIMEM_ALGO_COUNT] = {
    [MINIMEM_ALGO_SAME_PAGE]    = &minimem_same_page_compressor,
    [MINIMEM_ALGO_BDI]          = &minimem_bdi_compressor,
    [MINIMEM_ALGO_WKDM]         = &minimem_wkdm_compressor,
    [MINIMEM_ALGO_LZ4]          = &minimem_lz4_compressor,
    [MINIMEM_ALGO_LZSSE8]       = &minimem_lzsse8_compressor,
    [MINIMEM_ALGO_ZSTD_DICT]    = &minimem_zstd_dict_compressor,
    [MINIMEM_ALGO_DELTA]        = &minimem_delta_compressor,
    [MINIMEM_ALGO_WKDM64]       = &minimem_wkdm64_compressor,
    [MINIMEM_ALGO_BLOCK_CLASS]  = &minimem_block_class_compressor,
};

const struct minimem_compressor *minimem_get_compressor(int algo_id)
{
    if (algo_id < 0 || algo_id >= MINIMEM_ALGO_COUNT)
        return NULL;
    return compressors[algo_id];
}