// zfex from the submodule: a block of k = 8 data packets plus n - k = 4 FEC packets, as the link
// uses it (protocol table). Losing any 4 packets of the 12 must still recover every data packet.
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "check.hpp"
#include "zfex.h"

static const int k = 8, n = 12, size = 1024;  // size: a multiple of ZFEX_SIMD_ALIGNMENT

static uint8_t *alloc_block()
{
    return static_cast<uint8_t *>(std::aligned_alloc(ZFEX_SIMD_ALIGNMENT, size));
}

int main()
{
    fec_t *fec = nullptr;
    CHECK(fec_new(k, n, &fec) == ZFEX_SC_OK);

    uint8_t *block[n];
    for (int i = 0; i < n; i++)
    {
        block[i] = alloc_block();
    }
    srand(1);
    for (int i = 0; i < k; i++)
    {
        for (int j = 0; j < size; j++)
        {
            block[i][j] = static_cast<uint8_t>(rand());
        }
    }
    CHECK(fec_encode_simd(fec, block, block + k, size) == ZFEX_SC_OK);

    // Lose data packets 1, 3, 5 and 7: the four FEC packets take their places.
    const int lost[n - k] = {1, 3, 5, 7};
    const uint8_t *in[k];
    unsigned index[k];
    for (int i = 0; i < k; i++)
    {
        in[i] = block[i];
        index[i] = i;
    }
    for (int i = 0; i < n - k; i++)
    {
        in[lost[i]] = block[k + i];
        index[lost[i]] = k + i;
    }
    uint8_t *out[n - k];
    for (int i = 0; i < n - k; i++)
    {
        out[i] = alloc_block();
    }
    CHECK(fec_decode_simd(fec, in, out, index, size) == ZFEX_SC_OK);
    for (int i = 0; i < n - k; i++)
    {
        CHECK(std::memcmp(out[i], block[lost[i]], size) == 0);
    }

    for (int i = 0; i < n; i++)
    {
        std::free(block[i]);
    }
    for (int i = 0; i < n - k; i++)
    {
        std::free(out[i]);
    }
    fec_free(fec);
    return check_result("test_fec");
}
