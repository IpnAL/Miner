#include <memory.h>

#include "miner.h"
#include "algo-gate-api.h"

#include "algo/blake/sph_blake.h"
#include "algo/cubehash/sph_cubehash.h"
#include "algo/keccak/sph_keccak.h"
#include "algo/skein/sph_skein.h"
#include "algo/bmw/sph_bmw.h"
#include "algo/cubehash/sse2/cubehash_sse2.h" 
#include "lyra2.h"
#include "avxdefs.h"

// This gets allocated when miner_thread starts up and is never freed.
// It's not a leak because the only way to allocate it again is to exit
// the thread and that only occurs when the entire program exits.
__thread uint64_t* l2v2_wholeMatrix;

typedef struct {
        cubehashParam           cube1;
        cubehashParam           cube2;
        sph_blake256_context     blake;
        sph_keccak256_context    keccak;
        sph_skein256_context     skein;
        sph_bmw256_context       bmw;

} lyra2v2_ctx_holder;

static lyra2v2_ctx_holder lyra2v2_ctx;
static __thread sph_blake256_context l2v2_blake_mid;

void init_lyra2rev2_ctx()
{
        cubehashInit( &lyra2v2_ctx.cube1, 256, 16, 32 );
        cubehashInit( &lyra2v2_ctx.cube2, 256, 16, 32 );
        sph_blake256_init( &lyra2v2_ctx.blake );
        sph_keccak256_init( &lyra2v2_ctx.keccak );
        sph_skein256_init( &lyra2v2_ctx.skein );
        sph_bmw256_init( &lyra2v2_ctx.bmw );
}

void l2v2_blake256_midstate( const void* input )
{
    memcpy( &l2v2_blake_mid, &lyra2v2_ctx.blake, sizeof l2v2_blake_mid );
    sph_blake256( &l2v2_blake_mid, input, 64 );
}

void lyra2rev2_hash( void *state, const void *input )
{
        lyra2v2_ctx_holder ctx;
        memcpy( &ctx, &lyra2v2_ctx, sizeof(lyra2v2_ctx) );
	uint32_t _ALIGN(128) hashA[8], hashB[8];

        const int midlen = 64;            // bytes
        const int tail   = 80 - midlen;   // 16

        memcpy( &ctx.blake, &l2v2_blake_mid, sizeof l2v2_blake_mid );
	sph_blake256( &ctx.blake, (uint8_t*)input + midlen, tail );
	sph_blake256_close( &ctx.blake, hashA );

	sph_keccak256( &ctx.keccak, hashA, 32 );
	sph_keccak256_close(&ctx.keccak, hashB);

        cubehashUpdateDigest( &ctx.cube1, (byte*) hashA,
                              (const byte*) hashB, 32 );

	LYRA2REV2( l2v2_wholeMatrix, hashA, 32, hashA, 32, hashA, 32, 1, 4, 4 );

	sph_skein256( &ctx.skein, hashA, 32 );
	sph_skein256_close( &ctx.skein, hashB );

        cubehashUpdateDigest( &ctx.cube2, (byte*) hashA, 
                              (const byte*) hashB, 32 );

	sph_bmw256( &ctx.bmw, hashA, 32 );
	sph_bmw256_close( &ctx.bmw, hashB );

	memcpy( state, hashB, 32 );
}

int scanhash_lyra2rev2(int thr_id, struct work *work,
	uint32_t max_nonce, uint64_t *hashes_done)
{
        uint32_t *pdata = work->data;
        uint32_t *ptarget = work->target;
	uint32_t _ALIGN(64) endiandata[20];
        uint32_t hash[8] __attribute__((aligned(32)));
	const uint32_t first_nonce = pdata[19];
	uint32_t nonce = first_nonce;
        const uint32_t Htarg = ptarget[7];

	if (opt_benchmark)
		((uint32_t*)ptarget)[7] = 0x0000ff;

        swab32_array( endiandata, pdata, 20 );

        l2v2_blake256_midstate( endiandata );

	do {
		be32enc(&endiandata[19], nonce);
		lyra2rev2_hash(hash, endiandata);

		if (hash[7] <= Htarg )
                {
                   if( fulltest(hash, ptarget) )
                   {
			pdata[19] = nonce;
			*hashes_done = pdata[19] - first_nonce;
		   	return 1;
		   }
                }
		nonce++;

	} while (nonce < max_nonce && !work_restart[thr_id].restart);

	pdata[19] = nonce;
	*hashes_done = pdata[19] - first_nonce + 1;
	return 0;
}

void lyra2rev2_set_target( struct work* work, double job_diff )
{
 work_set_target( work, job_diff / (256.0 * opt_diff_factor) );
}


bool lyra2rev2_thread_init()
{
   const int64_t ROW_LEN_INT64 = BLOCK_LEN_INT64 * 4; // nCols
   const int64_t ROW_LEN_BYTES = ROW_LEN_INT64 * 8;

   int i = (int64_t)ROW_LEN_BYTES * 4; // nRows;
   l2v2_wholeMatrix = _mm_malloc( i, 64 );

   if ( l2v2_wholeMatrix == NULL )
     return false;

#if defined (__AVX2__)
   memset_zero_m256i( (__m256i*)l2v2_wholeMatrix, i/32 );
#elif defined(__AVX__)
   memset_zero_m128i( (__m128i*)l2v2_wholeMatrix, i/16 );
#else
   memset( l2v2_wholeMatrix, 0, i );
#endif
   return true;
}

bool register_lyra2rev2_algo( algo_gate_t* gate )
{
  init_lyra2rev2_ctx();
  gate->optimizations = SSE2_OPT | AES_OPT | AVX_OPT | AVX2_OPT;
  gate->miner_thread_init = (void*)&lyra2rev2_thread_init;
  gate->scanhash   = (void*)&scanhash_lyra2rev2;
  gate->hash       = (void*)&lyra2rev2_hash;
  gate->hash_alt   = (void*)&lyra2rev2_hash;
  gate->set_target = (void*)&lyra2rev2_set_target;
  return true;
};

