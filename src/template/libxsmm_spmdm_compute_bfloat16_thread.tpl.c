/******************************************************************************
** Copyright (c) 2016-2019, Intel Corporation                                **
** All rights reserved.                                                      **
**                                                                           **
** Redistribution and use in source and binary forms, with or without        **
** modification, are permitted provided that the following conditions        **
** are met:                                                                  **
** 1. Redistributions of source code must retain the above copyright         **
**    notice, this list of conditions and the following disclaimer.          **
** 2. Redistributions in binary form must reproduce the above copyright      **
**    notice, this list of conditions and the following disclaimer in the    **
**    documentation and/or other materials provided with the distribution.   **
** 3. Neither the name of the copyright holder nor the names of its          **
**    contributors may be used to endorse or promote products derived        **
**    from this software without specific prior written permission.          **
**                                                                           **
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS       **
** "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT         **
** LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR     **
** A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT      **
** HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,    **
** SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED  **
** TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR    **
** PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF    **
** LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING      **
** NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS        **
** SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.              **
******************************************************************************/
/* Nadathur Satish (Intel Corp.)
******************************************************************************/

const int m_blocks = handle->mb;
/*const int n_blocks = handle->nb;*/
const int k_blocks = handle->kb;
const int m_block_size = handle->bm;
const int n_block_size = handle->bn;
const int k_block_size = handle->bk;
int mb = block_id / handle->nb;
int nb = block_id % handle->nb;


#define LIBXSMM_SPMDM_COMPUTE_NREGS (6)
int m_overall_start = mb*m_block_size;
int m_overall_end   = (mb + 1)*m_block_size;
int num_m;
int num_m_aligned;

int n_overall_start = nb*n_block_size;
int n_overall_end   = (nb + 1)*n_block_size;
int num_n;
int m, n, k, kb;
int last_block_n, num_full_regs, last_n_start;

int k_overall_start, k_overall_end, num_k;

float *const scratch_C = (float *)(handle->base_ptr_scratch_B_scratch_C + (size_t)tid*handle->memory_for_scratch_per_thread);
float *const scratch_B = (float *)(handle->base_ptr_scratch_B_scratch_C + (size_t)tid*handle->memory_for_scratch_per_thread + (size_t)m_block_size*n_block_size*sizeof(float));
#if 0
float *const scratch_C = (float *)(handle->spmdm_scratch_C + tid*m_block_size*n_block_size*sizeof(float));
float *const scratch_B = (float *)(handle->spmdm_scratch_B + tid*k_block_size*n_block_size*sizeof(float));
#endif

SIMDTYPE_FP32 sum[2*LIBXSMM_SPMDM_COMPUTE_NREGS];
float* LIBXSMM_RESTRICT ptr_result;
#if SIMD_WIDTH_FP32 > 1
SIMDTYPE_INT32 vzero = _MM_SETZERO_INT32();
#endif

LIBXSMM_UNUSED(nthreads);
LIBXSMM_UNUSED(transa);
LIBXSMM_UNUSED(alpha);
LIBXSMM_UNUSED(beta);
LIBXSMM_UNUSED(tid);

/* really is twice this */
assert(n_block_size == LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32);

if (m_overall_end > handle->m) m_overall_end = handle->m;
num_m = (m_overall_end - m_overall_start);
num_m_aligned = (num_m / 2) * 2;

if (n_overall_end > handle->n) n_overall_end = handle->n;
num_n = (n_overall_end - n_overall_start);
last_block_n = (num_n != n_block_size);
num_full_regs = (num_n / SIMD_WIDTH_FP32);
if ((num_full_regs > 0) && (num_full_regs%2)) num_full_regs--;
last_n_start = num_full_regs*SIMD_WIDTH_FP32;

/* Copy in c matrix to buffer */
ptr_result = c + (size_t)m_overall_start*handle->n + n_overall_start;
if (LIBXSMM_FEQ(0.f, *beta)) {
  if (!last_block_n) {
    for (m = 0; m < num_m; m++) {
      _MM_STORE_FP32(scratch_C + (size_t)m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + 0*SIMD_WIDTH_FP32, _MM_SETZERO_FP32());
      _MM_STORE_FP32(scratch_C + (size_t)m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + 1*SIMD_WIDTH_FP32, _MM_SETZERO_FP32());
      _MM_STORE_FP32(scratch_C + (size_t)m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + 2*SIMD_WIDTH_FP32, _MM_SETZERO_FP32());
      _MM_STORE_FP32(scratch_C + (size_t)m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + 3*SIMD_WIDTH_FP32, _MM_SETZERO_FP32());
      _MM_STORE_FP32(scratch_C + (size_t)m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + 4*SIMD_WIDTH_FP32, _MM_SETZERO_FP32());
      _MM_STORE_FP32(scratch_C + (size_t)m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + 5*SIMD_WIDTH_FP32, _MM_SETZERO_FP32());
    }
  } else {
    for (m = 0; m < num_m; m++) {
      for (n = 0; n < num_full_regs; n += 2) {
        _MM_STORE_FP32(scratch_C + (size_t)m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + ((size_t)n)*SIMD_WIDTH_FP32, _MM_SETZERO_FP32());
        _MM_STORE_FP32(scratch_C + (size_t)m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + ((size_t)n+1)*SIMD_WIDTH_FP32, _MM_SETZERO_FP32());
      }
      for (n = last_n_start; n < num_n; n++) {
        scratch_C[m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + n] = 0;
      }
    }
  }
}
else if (LIBXSMM_FEQ(1.f, *beta)) {
  if ('T' == transc || 't' == transc) {
    int num_m_simd = num_m / SIMD_WIDTH_FP32 * SIMD_WIDTH_FP32;
    int num_n_simd = num_n / SIMD_WIDTH_FP32 * SIMD_WIDTH_FP32;
    int m2;

    ptr_result = c + (size_t)n_overall_start*handle->m + m_overall_start;

    for (m = 0; m < num_m_simd; m += SIMD_WIDTH_FP32) {
      for (n = 0; n < num_n_simd; n += SIMD_WIDTH_FP32) {
        TRANSPOSE_SIMD_WIDTH_KERNEL(ptr_result + (size_t)n*handle->m + m, handle->m, scratch_C + (size_t)m*n_block_size + n, n_block_size);
      }
      /* Transpose a SIMD_WIDTH_FP32 * (num_n - num_n_simd) block of output space - input is of size (num_n - num_n_simd) * SIMD_WIDTH_FP32 */
      for (m2 = m; m2 < m + SIMD_WIDTH_FP32; m2++) {
        for (n = num_n_simd; n < num_n; n++) {
          scratch_C[m2*n_block_size + n] = ptr_result[n*handle->m + m2];
        }
      }
    }
    /* Transpose a (num_m - num_m_simd) * num_n block of output space - input is of size num_n * (num_m - num_m_simd) */
    for (m = num_m_simd; m < num_m; m++) {
      for (n = 0; n < num_n; n++) {
        scratch_C[m*n_block_size + n] = ptr_result[n*handle->m + m];
      }
    }
  }
  else {
    if (!last_block_n) {
      for (m = 0; m < num_m; m++) {
        _MM_STORE_FP32(scratch_C + (size_t)m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + 0*SIMD_WIDTH_FP32, _MM_LOADU_FP32(ptr_result + (size_t)m*handle->n + 0*SIMD_WIDTH_FP32));
        _MM_STORE_FP32(scratch_C + (size_t)m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + 1*SIMD_WIDTH_FP32, _MM_LOADU_FP32(ptr_result + (size_t)m*handle->n + 1*SIMD_WIDTH_FP32));
        _MM_STORE_FP32(scratch_C + (size_t)m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + 2*SIMD_WIDTH_FP32, _MM_LOADU_FP32(ptr_result + (size_t)m*handle->n + 2*SIMD_WIDTH_FP32));
        _MM_STORE_FP32(scratch_C + (size_t)m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + 3*SIMD_WIDTH_FP32, _MM_LOADU_FP32(ptr_result + (size_t)m*handle->n + 3*SIMD_WIDTH_FP32));
        _MM_STORE_FP32(scratch_C + (size_t)m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + 4*SIMD_WIDTH_FP32, _MM_LOADU_FP32(ptr_result + (size_t)m*handle->n + 4*SIMD_WIDTH_FP32));
        _MM_STORE_FP32(scratch_C + (size_t)m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + 5*SIMD_WIDTH_FP32, _MM_LOADU_FP32(ptr_result + (size_t)m*handle->n + 5*SIMD_WIDTH_FP32));
      }
    }
    else {
      for (m = 0; m < num_m; m++) {
        for (n = 0; n < num_full_regs; n += 2) {
          _MM_STORE_FP32(scratch_C + (size_t)m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + ((size_t)n)  *SIMD_WIDTH_FP32, _MM_LOADU_FP32(ptr_result + (size_t)m*handle->n + ((size_t)n)  *SIMD_WIDTH_FP32));
          _MM_STORE_FP32(scratch_C + (size_t)m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + ((size_t)n+1)*SIMD_WIDTH_FP32, _MM_LOADU_FP32(ptr_result + (size_t)m*handle->n + ((size_t)n+1)*SIMD_WIDTH_FP32));
        }
        for (n = last_n_start; n < num_n; n++) {
          scratch_C[m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32+n] = ptr_result[m*handle->n+n];
        }
      }
    }
  }
}
else {
  SIMDTYPE_FP32 beta_v = _MM_SET1_FP32(*beta);
  if ('T' == transc || 't' == transc) {
    int num_m_simd = num_m / SIMD_WIDTH_FP32 * SIMD_WIDTH_FP32;
    int num_n_simd = num_n / SIMD_WIDTH_FP32 * SIMD_WIDTH_FP32;
    int m2;

    ptr_result = c + (size_t)n_overall_start*handle->m + m_overall_start;

    for (m = 0; m < num_m_simd; m += SIMD_WIDTH_FP32) {
      for (n = 0; n < num_n_simd; n += SIMD_WIDTH_FP32) {
        TRANSPOSE_SIMD_WIDTH_KERNEL(ptr_result + (size_t)n*handle->m + m, handle->m, scratch_C + (size_t)m*n_block_size + n, n_block_size);
        _MM_STORE_FP32(scratch_C + (size_t)m*n_block_size + n, _MM_MUL_FP32(beta_v, _MM_LOADU_FP32(scratch_C + (size_t)m*n_block_size + n)));
        _MM_STORE_FP32(scratch_C + (size_t)m*n_block_size + n + (size_t)n_block_size*1, _MM_MUL_FP32(beta_v, _MM_LOADU_FP32(scratch_C + (size_t)m*n_block_size + n + (size_t)n_block_size*1)));
        _MM_STORE_FP32(scratch_C + (size_t)m*n_block_size + n + (size_t)n_block_size*2, _MM_MUL_FP32(beta_v, _MM_LOADU_FP32(scratch_C + (size_t)m*n_block_size + n + (size_t)n_block_size*2)));
        _MM_STORE_FP32(scratch_C + (size_t)m*n_block_size + n + (size_t)n_block_size*3, _MM_MUL_FP32(beta_v, _MM_LOADU_FP32(scratch_C + (size_t)m*n_block_size + n + (size_t)n_block_size*3)));
        _MM_STORE_FP32(scratch_C + (size_t)m*n_block_size + n + (size_t)n_block_size*4, _MM_MUL_FP32(beta_v, _MM_LOADU_FP32(scratch_C + (size_t)m*n_block_size + n + (size_t)n_block_size*4)));
        _MM_STORE_FP32(scratch_C + (size_t)m*n_block_size + n + (size_t)n_block_size*5, _MM_MUL_FP32(beta_v, _MM_LOADU_FP32(scratch_C + (size_t)m*n_block_size + n + (size_t)n_block_size*5)));
        _MM_STORE_FP32(scratch_C + (size_t)m*n_block_size + n + (size_t)n_block_size*6, _MM_MUL_FP32(beta_v, _MM_LOADU_FP32(scratch_C + (size_t)m*n_block_size + n + (size_t)n_block_size*6)));
        _MM_STORE_FP32(scratch_C + (size_t)m*n_block_size + n + (size_t)n_block_size*7, _MM_MUL_FP32(beta_v, _MM_LOADU_FP32(scratch_C + (size_t)m*n_block_size + n + (size_t)n_block_size*7)));
      }
      /* Transpose a SIMD_WIDTH_FP32 * (num_n - num_n_simd) block of output space - input is of size (num_n - num_n_simd) * SIMD_WIDTH_FP32 */
      for (m2 = m; m2 < m + SIMD_WIDTH_FP32; m2++) {
        for (n = num_n_simd; n < num_n; n++) {
          scratch_C[m2*n_block_size + n] = (*beta)*ptr_result[n*handle->m + m2];
        }
      }
    }
    /* Transpose a (num_m - num_m_simd) * num_n block of output space - input is of size num_n * (num_m - num_m_simd) */
    for (m = num_m_simd; m < num_m; m++) {
      for (n = 0; n < num_n; n++) {
        scratch_C[m*n_block_size + n] = (*beta)*ptr_result[n*handle->m + m];
      }
    }

  }
  else {
    if (!last_block_n) {
      for (m = 0; m < num_m; m++) {
        _MM_STORE_FP32(scratch_C + (size_t)m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + 0*SIMD_WIDTH_FP32, _MM_MUL_FP32(beta_v, _MM_LOADU_FP32(ptr_result + (size_t)m*handle->n + 0*SIMD_WIDTH_FP32)));
        _MM_STORE_FP32(scratch_C + (size_t)m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + 1*SIMD_WIDTH_FP32, _MM_MUL_FP32(beta_v, _MM_LOADU_FP32(ptr_result + (size_t)m*handle->n + 1*SIMD_WIDTH_FP32)));
        _MM_STORE_FP32(scratch_C + (size_t)m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + 2*SIMD_WIDTH_FP32, _MM_MUL_FP32(beta_v, _MM_LOADU_FP32(ptr_result + (size_t)m*handle->n + 2*SIMD_WIDTH_FP32)));
        _MM_STORE_FP32(scratch_C + (size_t)m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + 3*SIMD_WIDTH_FP32, _MM_MUL_FP32(beta_v, _MM_LOADU_FP32(ptr_result + (size_t)m*handle->n + 3*SIMD_WIDTH_FP32)));
        _MM_STORE_FP32(scratch_C + (size_t)m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + 4*SIMD_WIDTH_FP32, _MM_MUL_FP32(beta_v, _MM_LOADU_FP32(ptr_result + (size_t)m*handle->n + 4*SIMD_WIDTH_FP32)));
        _MM_STORE_FP32(scratch_C + (size_t)m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + 5*SIMD_WIDTH_FP32, _MM_MUL_FP32(beta_v, _MM_LOADU_FP32(ptr_result + (size_t)m*handle->n + 5*SIMD_WIDTH_FP32)));
      }
    }
    else {
      for (m = 0; m < num_m; m++) {
        for (n = 0; n < num_full_regs; n += 2) {
          _MM_STORE_FP32(scratch_C + (size_t)m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + ((size_t)n)  *SIMD_WIDTH_FP32, _MM_MUL_FP32(beta_v, _MM_LOADU_FP32(ptr_result + (size_t)m*handle->n + ((size_t)n)  *SIMD_WIDTH_FP32)));
          _MM_STORE_FP32(scratch_C + (size_t)m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + ((size_t)n+1)*SIMD_WIDTH_FP32, _MM_MUL_FP32(beta_v, _MM_LOADU_FP32(ptr_result + (size_t)m*handle->n + ((size_t)n+1)*SIMD_WIDTH_FP32)));
        }
        for (n = last_n_start; n < num_n; n++) {
          scratch_C[m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + n] = (*beta)*ptr_result[m*handle->n + n];
        }
      }
    }
  }
}

for (kb = 0; kb < k_blocks; kb++) {
  const uint16_t* LIBXSMM_RESTRICT ptr_dense;
  float * LIBXSMM_RESTRICT scratch_C_base;
  const float * LIBXSMM_RESTRICT scratch_B_base;
  int block_A = kb * m_blocks + mb;
  libxsmm_CSR_sparseslice slice = a_sparse[block_A];
  int m_local = 0;

  k_overall_start = kb*k_block_size;
  k_overall_end   = (kb+1)*k_block_size;
  num_k = (k_overall_end - k_overall_start);

  /* Copy in b matrix */
  if ('T' == transb || 't' == transb) {
    int num_k_simd = num_k / SIMD_WIDTH_FP32 * SIMD_WIDTH_FP32;
    int num_n_simd = num_n / SIMD_WIDTH_FP32 * SIMD_WIDTH_FP32;
    int k2;

    ptr_dense = b + (size_t)n_overall_start*handle->k + k_overall_start;

    for (k = 0; k < num_k_simd; k += SIMD_WIDTH_FP32) {
      for (n = 0; n < num_n_simd; n += SIMD_WIDTH_FP32) {
        TRANSPOSE_SIMD_WIDTH_KERNEL_BFLOAT16(ptr_dense + (size_t)n*handle->k + k, handle->k, scratch_B + (size_t)k*n_block_size + n, n_block_size);
      }
      /* Transpose a SIMD_WIDTH_FP32 * (num_n - num_n_simd) block of output space - input is of size (num_n - num_n_simd) * SIMD_WIDTH_FP32 */
      for (k2 = k; k2 < k + SIMD_WIDTH_FP32; k2++) {
        for (n = num_n_simd; n < num_n; n++) {
          uint16_t restmp = ptr_dense[n*handle->k + k2];
          union { int i; float f; } res;
          res.i = restmp;
          res.i <<= 16;
          scratch_B[k2*n_block_size + n] = res.f;
        }
      }
    }
    /* Transpose a (num_m - num_m_simd) * num_n block of output space - input is of size num_n * (num_m - num_m_simd) */
    for (k = num_k_simd; k < num_k; k++) {
      for (n = 0; n < num_n; n++) {
        uint16_t restmp = ptr_dense[n*handle->k + k];
        union { int i; float f; } res;
        res.i = restmp;
        res.i <<= 16;
        scratch_B[k*n_block_size + n] = res.f;
      }
    }
  } else {
    ptr_dense = b + (size_t)k_overall_start*handle->n + n_overall_start;
    if (!last_block_n) {
      for (k = 0; k < num_k; k++) {
        SIMDTYPE_INT32 vload_0 = _MM_LOADU_INT32((const SIMDTYPE_INT32*)(ptr_dense + (size_t)k*handle->n + 2*0*SIMD_WIDTH_FP32));
        SIMDTYPE_INT32 vload_1, vload_2;
        SIMDTYPE_FP32 v1_0, v2_0;
        SIMDTYPE_FP32 v1_1, v2_1;
        SIMDTYPE_FP32 v1_2, v2_2;
        EXPAND_BFLOAT16(vload_0, v1_0, v2_0);
        _MM_STORE_FP32(scratch_B + (size_t)k*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + 2*0*SIMD_WIDTH_FP32, v1_0);
        _MM_STORE_FP32(scratch_B + (size_t)k*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + (2*0+1)*SIMD_WIDTH_FP32, v2_0);
        vload_1 = _MM_LOADU_INT32((const SIMDTYPE_INT32 *)(ptr_dense + (size_t)k*handle->n + 2*1*SIMD_WIDTH_FP32));
        EXPAND_BFLOAT16(vload_1, v1_1, v2_1);
        _MM_STORE_FP32(scratch_B + (size_t)k*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + 2*1*SIMD_WIDTH_FP32, v1_1);
        _MM_STORE_FP32(scratch_B + (size_t)k*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + (2*1+1)*SIMD_WIDTH_FP32, v2_1);
        vload_2 = _MM_LOADU_INT32((const SIMDTYPE_INT32 *)(ptr_dense + (size_t)k*handle->n + 2*2*SIMD_WIDTH_FP32));
        EXPAND_BFLOAT16(vload_2, v1_2, v2_2);
        _MM_STORE_FP32(scratch_B + (size_t)k*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + 2*2*SIMD_WIDTH_FP32, v1_2);
        _MM_STORE_FP32(scratch_B + (size_t)k*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + (2*2+1)*SIMD_WIDTH_FP32, v2_2);
      }
    } else {
      for (k = 0; k < num_k; k++) {
        for (n = 0; n < num_full_regs; n += 2) {
          SIMDTYPE_INT32 vload_0 = _MM_LOADU_INT32((const SIMDTYPE_INT32*)(ptr_dense + (size_t)k*handle->n + (size_t)n*SIMD_WIDTH_FP32));
          SIMDTYPE_FP32 v1_0, v2_0;
          EXPAND_BFLOAT16(vload_0, v1_0, v2_0);
          _MM_STORE_FP32(scratch_B + (size_t)k*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + ((size_t)n)  *SIMD_WIDTH_FP32, v1_0);
          _MM_STORE_FP32(scratch_B + (size_t)k*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + ((size_t)n+1)*SIMD_WIDTH_FP32, v2_0);
        }
        for (n = last_n_start; n < num_n; n++) {
          uint16_t restmp = ptr_dense[k*handle->n + n];
          union { int i; float f; } res;
          res.i = restmp;
          res.i <<= 16;
          {
            scratch_B[k*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + n] = res.f;
          }
        }
      }
    }
  }

  scratch_C_base = scratch_C - (size_t)m_overall_start*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32;
  scratch_B_base = scratch_B; /* - (size_t)k_overall_start*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32; */

  for (m = m_overall_start; m < m_overall_start + num_m_aligned; m += 2, m_local += 2) {
    int start_j, end_j, end_j_2, num_j, num_j_2;
    const uint16_t *LIBXSMM_RESTRICT sp_c_ptr_base;
    const uint16_t *LIBXSMM_RESTRICT sp_c_ptr_base_2;
    const float *LIBXSMM_RESTRICT sp_v_ptr_base;
    const float *LIBXSMM_RESTRICT sp_v_ptr_base_2;
    float *const LIBXSMM_RESTRICT result_m_index   = scratch_C_base + ((size_t)m)  *LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32;
    float *const LIBXSMM_RESTRICT result_m_index_2 = scratch_C_base + ((size_t)m+1)*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32;

    if (m_local >= m_block_size) { block_A++; slice = a_sparse[block_A]; m_local = 0; }

    start_j = slice.rowidx[m_local];
    end_j   = slice.rowidx[m_local + 1];
    end_j_2 = slice.rowidx[m_local + 2];
    num_j   = (end_j - start_j);
    num_j_2 = (end_j_2 - end_j);
    sp_c_ptr_base = slice.colidx + start_j;
    sp_c_ptr_base_2 = slice.colidx + end_j;
    sp_v_ptr_base = (float *)(slice.values) + start_j;
    sp_v_ptr_base_2 = (float *)(slice.values) + end_j;

    if (!last_block_n)
    {
      int64_t j = 0, j2 = 0;
      sum[0] = _MM_LOAD_FP32(result_m_index + 0*SIMD_WIDTH_FP32);
      sum[0+LIBXSMM_SPMDM_COMPUTE_NREGS] = _MM_LOAD_FP32(result_m_index_2 + 0*SIMD_WIDTH_FP32);
      sum[1] = _MM_LOAD_FP32(result_m_index + 1*SIMD_WIDTH_FP32);
      sum[1+LIBXSMM_SPMDM_COMPUTE_NREGS] = _MM_LOAD_FP32(result_m_index_2 + 1*SIMD_WIDTH_FP32);
      sum[2] = _MM_LOAD_FP32(result_m_index + 2*SIMD_WIDTH_FP32);
      sum[2+LIBXSMM_SPMDM_COMPUTE_NREGS] = _MM_LOAD_FP32(result_m_index_2 + 2*SIMD_WIDTH_FP32);
      sum[3] = _MM_LOAD_FP32(result_m_index + 3*SIMD_WIDTH_FP32);
      sum[3+LIBXSMM_SPMDM_COMPUTE_NREGS] = _MM_LOAD_FP32(result_m_index_2 + 3*SIMD_WIDTH_FP32);
      sum[4] = _MM_LOAD_FP32(result_m_index + 4*SIMD_WIDTH_FP32);
      sum[4+LIBXSMM_SPMDM_COMPUTE_NREGS] = _MM_LOAD_FP32(result_m_index_2 + 4*SIMD_WIDTH_FP32);
      sum[5] = _MM_LOAD_FP32(result_m_index + 5*SIMD_WIDTH_FP32);
      sum[5+LIBXSMM_SPMDM_COMPUTE_NREGS] = _MM_LOAD_FP32(result_m_index_2 + 5*SIMD_WIDTH_FP32);
      for (; j < num_j && j2 < num_j_2; j++, j2++) {
        const float *const LIBXSMM_RESTRICT sp_col_dense_index   = scratch_B_base + (size_t)sp_c_ptr_base[j]   *LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32;
        const float *const LIBXSMM_RESTRICT sp_col_dense_index_2 = scratch_B_base + (size_t)sp_c_ptr_base_2[j2]*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32;
        SIMDTYPE_FP32 v_v = _MM_SET1_FP32(sp_v_ptr_base[j]);
        SIMDTYPE_FP32 v_v_2 = _MM_SET1_FP32(sp_v_ptr_base_2[j2]);
        sum[0] = _MM_FMADD_FP32(v_v, _MM_LOAD_FP32(sp_col_dense_index + 0*SIMD_WIDTH_FP32), sum[0]);
        sum[0 + LIBXSMM_SPMDM_COMPUTE_NREGS] = _MM_FMADD_FP32(v_v_2, _MM_LOAD_FP32(sp_col_dense_index_2 + 0*SIMD_WIDTH_FP32), sum[0+LIBXSMM_SPMDM_COMPUTE_NREGS]);
        sum[1] = _MM_FMADD_FP32(v_v, _MM_LOAD_FP32(sp_col_dense_index + 1*SIMD_WIDTH_FP32), sum[1]);
        sum[1 + LIBXSMM_SPMDM_COMPUTE_NREGS] = _MM_FMADD_FP32(v_v_2, _MM_LOAD_FP32(sp_col_dense_index_2 + 1*SIMD_WIDTH_FP32), sum[1+LIBXSMM_SPMDM_COMPUTE_NREGS]);
        sum[2] = _MM_FMADD_FP32(v_v, _MM_LOAD_FP32(sp_col_dense_index + 2*SIMD_WIDTH_FP32), sum[2]);
        sum[2 + LIBXSMM_SPMDM_COMPUTE_NREGS] = _MM_FMADD_FP32(v_v_2, _MM_LOAD_FP32(sp_col_dense_index_2 + 2*SIMD_WIDTH_FP32), sum[2+LIBXSMM_SPMDM_COMPUTE_NREGS]);
        sum[3] = _MM_FMADD_FP32(v_v, _MM_LOAD_FP32(sp_col_dense_index + 3*SIMD_WIDTH_FP32), sum[3]);
        sum[3 + LIBXSMM_SPMDM_COMPUTE_NREGS] = _MM_FMADD_FP32(v_v_2, _MM_LOAD_FP32(sp_col_dense_index_2 + 3*SIMD_WIDTH_FP32), sum[3+LIBXSMM_SPMDM_COMPUTE_NREGS]);
        sum[4] = _MM_FMADD_FP32(v_v, _MM_LOAD_FP32(sp_col_dense_index + 4*SIMD_WIDTH_FP32), sum[4]);
        sum[4 + LIBXSMM_SPMDM_COMPUTE_NREGS] = _MM_FMADD_FP32(v_v_2, _MM_LOAD_FP32(sp_col_dense_index_2 + 4*SIMD_WIDTH_FP32), sum[4+LIBXSMM_SPMDM_COMPUTE_NREGS]);
        sum[5] = _MM_FMADD_FP32(v_v, _MM_LOAD_FP32(sp_col_dense_index + 5*SIMD_WIDTH_FP32), sum[5]);
        sum[5 + LIBXSMM_SPMDM_COMPUTE_NREGS] = _MM_FMADD_FP32(v_v_2, _MM_LOAD_FP32(sp_col_dense_index_2 + 5*SIMD_WIDTH_FP32), sum[5+LIBXSMM_SPMDM_COMPUTE_NREGS]);
      }
      for (; j < num_j; j++) {
        const float *const LIBXSMM_RESTRICT sp_col_dense_index = scratch_B_base + (size_t)sp_c_ptr_base[j]*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32;
        SIMDTYPE_FP32 v_v = _MM_SET1_FP32(sp_v_ptr_base[j]);
        sum[0] = _MM_FMADD_FP32(v_v, _MM_LOAD_FP32(sp_col_dense_index + 0*SIMD_WIDTH_FP32), sum[0]);
        sum[1] = _MM_FMADD_FP32(v_v, _MM_LOAD_FP32(sp_col_dense_index + 1*SIMD_WIDTH_FP32), sum[1]);
        sum[2] = _MM_FMADD_FP32(v_v, _MM_LOAD_FP32(sp_col_dense_index + 2*SIMD_WIDTH_FP32), sum[2]);
        sum[3] = _MM_FMADD_FP32(v_v, _MM_LOAD_FP32(sp_col_dense_index + 3*SIMD_WIDTH_FP32), sum[3]);
        sum[4] = _MM_FMADD_FP32(v_v, _MM_LOAD_FP32(sp_col_dense_index + 4*SIMD_WIDTH_FP32), sum[4]);
        sum[5] = _MM_FMADD_FP32(v_v, _MM_LOAD_FP32(sp_col_dense_index + 5*SIMD_WIDTH_FP32), sum[5]);
      }
      for (; j2 < num_j_2; j2++) {
        const float *const LIBXSMM_RESTRICT sp_col_dense_index_2 = scratch_B_base + (size_t)sp_c_ptr_base_2[j2]*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32;
        SIMDTYPE_FP32 v_v_2 = _MM_SET1_FP32(sp_v_ptr_base_2[j2]);
        sum[0 + LIBXSMM_SPMDM_COMPUTE_NREGS] = _MM_FMADD_FP32(v_v_2, _MM_LOAD_FP32(sp_col_dense_index_2 + 0*SIMD_WIDTH_FP32), sum[0+LIBXSMM_SPMDM_COMPUTE_NREGS]);
        sum[1 + LIBXSMM_SPMDM_COMPUTE_NREGS] = _MM_FMADD_FP32(v_v_2, _MM_LOAD_FP32(sp_col_dense_index_2 + 1*SIMD_WIDTH_FP32), sum[1+LIBXSMM_SPMDM_COMPUTE_NREGS]);
        sum[2 + LIBXSMM_SPMDM_COMPUTE_NREGS] = _MM_FMADD_FP32(v_v_2, _MM_LOAD_FP32(sp_col_dense_index_2 + 2*SIMD_WIDTH_FP32), sum[2+LIBXSMM_SPMDM_COMPUTE_NREGS]);
        sum[3 + LIBXSMM_SPMDM_COMPUTE_NREGS] = _MM_FMADD_FP32(v_v_2, _MM_LOAD_FP32(sp_col_dense_index_2 + 3*SIMD_WIDTH_FP32), sum[3+LIBXSMM_SPMDM_COMPUTE_NREGS]);
        sum[4 + LIBXSMM_SPMDM_COMPUTE_NREGS] = _MM_FMADD_FP32(v_v_2, _MM_LOAD_FP32(sp_col_dense_index_2 + 4*SIMD_WIDTH_FP32), sum[4+LIBXSMM_SPMDM_COMPUTE_NREGS]);
        sum[5 + LIBXSMM_SPMDM_COMPUTE_NREGS] = _MM_FMADD_FP32(v_v_2, _MM_LOAD_FP32(sp_col_dense_index_2 + 5*SIMD_WIDTH_FP32), sum[5+LIBXSMM_SPMDM_COMPUTE_NREGS]);
      }
      _MM_STORE_FP32(result_m_index + 0*SIMD_WIDTH_FP32, sum[0]);
      _MM_STORE_FP32(result_m_index_2 + 0*SIMD_WIDTH_FP32, sum[0+LIBXSMM_SPMDM_COMPUTE_NREGS]);
      _MM_STORE_FP32(result_m_index + 1*SIMD_WIDTH_FP32, sum[1]);
      _MM_STORE_FP32(result_m_index_2 + 1*SIMD_WIDTH_FP32, sum[1+LIBXSMM_SPMDM_COMPUTE_NREGS]);
      _MM_STORE_FP32(result_m_index + 2*SIMD_WIDTH_FP32, sum[2]);
      _MM_STORE_FP32(result_m_index_2 + 2*SIMD_WIDTH_FP32, sum[2+LIBXSMM_SPMDM_COMPUTE_NREGS]);
      _MM_STORE_FP32(result_m_index + 3*SIMD_WIDTH_FP32, sum[3]);
      _MM_STORE_FP32(result_m_index_2 + 3*SIMD_WIDTH_FP32, sum[3+LIBXSMM_SPMDM_COMPUTE_NREGS]);
      _MM_STORE_FP32(result_m_index + 4*SIMD_WIDTH_FP32, sum[4]);
      _MM_STORE_FP32(result_m_index_2 + 4*SIMD_WIDTH_FP32, sum[4+LIBXSMM_SPMDM_COMPUTE_NREGS]);
      _MM_STORE_FP32(result_m_index + 5*SIMD_WIDTH_FP32, sum[5]);
      _MM_STORE_FP32(result_m_index_2 + 5*SIMD_WIDTH_FP32, sum[5+LIBXSMM_SPMDM_COMPUTE_NREGS]);
    }
    else {
      int64_t j = 0, j2 = 0;
      for (n = 0; n < num_full_regs; n += 2) {
        sum[n] = _MM_SETZERO_FP32();
        sum[n+LIBXSMM_SPMDM_COMPUTE_NREGS] = _MM_SETZERO_FP32();
        sum[n+1] = _MM_SETZERO_FP32();
        sum[n+1+LIBXSMM_SPMDM_COMPUTE_NREGS] = _MM_SETZERO_FP32();
      }
      for (; j < num_j && j2 < num_j_2; j++, j2++) {
        const float *const LIBXSMM_RESTRICT sp_col_dense_index   = scratch_B_base + (size_t)sp_c_ptr_base[j]   *LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32;
        const float *const LIBXSMM_RESTRICT sp_col_dense_index_2 = scratch_B_base + (size_t)sp_c_ptr_base_2[j2]*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32;
        SIMDTYPE_FP32 v_v = _MM_SET1_FP32(sp_v_ptr_base[j]);
        SIMDTYPE_FP32 v_v_2 = _MM_SET1_FP32(sp_v_ptr_base_2[j2]);
        for (n = 0; n < num_full_regs; n += 2) {
          sum[n] = _MM_FMADD_FP32(v_v, _MM_LOAD_FP32(sp_col_dense_index + (size_t)n*SIMD_WIDTH_FP32), sum[n]);
          sum[n+LIBXSMM_SPMDM_COMPUTE_NREGS] = _MM_FMADD_FP32(v_v_2, _MM_LOAD_FP32(sp_col_dense_index_2 + (size_t)n*SIMD_WIDTH_FP32), sum[n+LIBXSMM_SPMDM_COMPUTE_NREGS]);
          sum[n+1] = _MM_FMADD_FP32(v_v, _MM_LOAD_FP32(sp_col_dense_index + ((size_t)n+1)*SIMD_WIDTH_FP32), sum[n+1]);
          sum[n+1 + LIBXSMM_SPMDM_COMPUTE_NREGS] = _MM_FMADD_FP32(v_v_2, _MM_LOAD_FP32(sp_col_dense_index_2 + ((size_t)n+1)*SIMD_WIDTH_FP32), sum[n+1+LIBXSMM_SPMDM_COMPUTE_NREGS]);
        }
        {
          float v_v_f = sp_v_ptr_base[j];
          float v_v_f_2 = sp_v_ptr_base_2[j2];
          for (n = last_n_start; n < num_n; n++) {
            result_m_index[n] += sp_col_dense_index[n]*v_v_f;
            result_m_index_2[n] += sp_col_dense_index_2[n]*v_v_f_2;
          }
        }
      }
      for (; j < num_j; j++) {
        const float *const LIBXSMM_RESTRICT sp_col_dense_index = scratch_B_base + (size_t)sp_c_ptr_base[j]*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32;
        SIMDTYPE_FP32 v_v = _MM_SET1_FP32(sp_v_ptr_base[j]);
        for (n = 0; n < num_full_regs; n += 2) {
          sum[n]   = _MM_FMADD_FP32(v_v, _MM_LOAD_FP32(sp_col_dense_index + ((size_t)n)  *SIMD_WIDTH_FP32), sum[n]);
          sum[n+1] = _MM_FMADD_FP32(v_v, _MM_LOAD_FP32(sp_col_dense_index + ((size_t)n+1)*SIMD_WIDTH_FP32), sum[n+1]);
        }
        {
          float v_v_f = sp_v_ptr_base[j];
          for (n = last_n_start; n < num_n; n++) {
            result_m_index[n] += sp_col_dense_index[n]*v_v_f;
          }
        }
      }
      for (; j2 < num_j_2; j2++) {
        const float *const LIBXSMM_RESTRICT sp_col_dense_index_2 = scratch_B_base + (size_t)sp_c_ptr_base_2[j2]*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32;
        SIMDTYPE_FP32 v_v_2 = _MM_SET1_FP32(sp_v_ptr_base_2[j2]);
        for (n = 0; n < num_full_regs; n += 2) {
          sum[n + LIBXSMM_SPMDM_COMPUTE_NREGS]   = _MM_FMADD_FP32(v_v_2, _MM_LOAD_FP32(sp_col_dense_index_2 + ((size_t)n)  *SIMD_WIDTH_FP32), sum[n+LIBXSMM_SPMDM_COMPUTE_NREGS]);
          sum[n+1 + LIBXSMM_SPMDM_COMPUTE_NREGS] = _MM_FMADD_FP32(v_v_2, _MM_LOAD_FP32(sp_col_dense_index_2 + ((size_t)n+1)*SIMD_WIDTH_FP32), sum[n+1+LIBXSMM_SPMDM_COMPUTE_NREGS]);
        }
        {
          float v_v_f_2 = sp_v_ptr_base_2[j2];
          for (n = last_n_start; n < num_n; n++) {
            result_m_index_2[n] += sp_col_dense_index_2[n]*v_v_f_2;
          }
        }
      }
      for (n = 0; n < num_full_regs; n += 2) {
        _MM_STORE_FP32(result_m_index   + ((size_t)n)  *SIMD_WIDTH_FP32, _MM_ADD_FP32(sum[n], _MM_LOAD_FP32(result_m_index + (size_t)n*SIMD_WIDTH_FP32)));
        _MM_STORE_FP32(result_m_index_2 + ((size_t)n)  *SIMD_WIDTH_FP32, _MM_ADD_FP32(sum[n+LIBXSMM_SPMDM_COMPUTE_NREGS], _MM_LOAD_FP32(result_m_index_2 + (size_t)n*SIMD_WIDTH_FP32)));
        _MM_STORE_FP32(result_m_index   + ((size_t)n+1)*SIMD_WIDTH_FP32, _MM_ADD_FP32(sum[n+1], _MM_LOAD_FP32(result_m_index + ((size_t)n+1)*SIMD_WIDTH_FP32)));
        _MM_STORE_FP32(result_m_index_2 + ((size_t)n+1)*SIMD_WIDTH_FP32, _MM_ADD_FP32(sum[n+1+LIBXSMM_SPMDM_COMPUTE_NREGS], _MM_LOAD_FP32(result_m_index_2 + ((size_t)n+1)*SIMD_WIDTH_FP32)));
      }
    }
  }
  for (m = m_overall_start + num_m_aligned; m < m_overall_end; m++, m_local++) {
    int start_j, end_j, num_j;
    const uint16_t*  LIBXSMM_RESTRICT sp_c_ptr_base;
    const float* LIBXSMM_RESTRICT sp_v_ptr_base;
    float* LIBXSMM_RESTRICT result_m_index;

    if (m_local >= m_block_size) { block_A++; slice = a_sparse[block_A]; m_local = 0; }

    start_j = slice.rowidx[m_local];
    end_j   = slice.rowidx[m_local + 1];
    num_j   = (end_j - start_j);
    sp_c_ptr_base = slice.colidx + start_j;
    sp_v_ptr_base = slice.values + start_j;
    result_m_index = scratch_C_base + (size_t)m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32;

    if (!last_block_n) {
      int64_t j = 0;
      sum[0] = _MM_LOAD_FP32(result_m_index + 0*SIMD_WIDTH_FP32);
      sum[1] = _MM_LOAD_FP32(result_m_index + 1*SIMD_WIDTH_FP32);
      sum[2] = _MM_LOAD_FP32(result_m_index + 2*SIMD_WIDTH_FP32);
      sum[3] = _MM_LOAD_FP32(result_m_index + 3*SIMD_WIDTH_FP32);
      sum[4] = _MM_LOAD_FP32(result_m_index + 4*SIMD_WIDTH_FP32);
      sum[5] = _MM_LOAD_FP32(result_m_index + 5*SIMD_WIDTH_FP32);
      for (; j < num_j; j++) {
        const float *const LIBXSMM_RESTRICT sp_col_dense_index = scratch_B_base + (size_t)sp_c_ptr_base[j]*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32;
        SIMDTYPE_FP32 v_v = _MM_SET1_FP32(sp_v_ptr_base[j]);
        sum[0] = _MM_FMADD_FP32(v_v, _MM_LOAD_FP32(sp_col_dense_index + 0*SIMD_WIDTH_FP32), sum[0]);
        sum[1] = _MM_FMADD_FP32(v_v, _MM_LOAD_FP32(sp_col_dense_index + 1*SIMD_WIDTH_FP32), sum[1]);
        sum[2] = _MM_FMADD_FP32(v_v, _MM_LOAD_FP32(sp_col_dense_index + 2*SIMD_WIDTH_FP32), sum[2]);
        sum[3] = _MM_FMADD_FP32(v_v, _MM_LOAD_FP32(sp_col_dense_index + 3*SIMD_WIDTH_FP32), sum[3]);
        sum[4] = _MM_FMADD_FP32(v_v, _MM_LOAD_FP32(sp_col_dense_index + 4*SIMD_WIDTH_FP32), sum[4]);
        sum[5] = _MM_FMADD_FP32(v_v, _MM_LOAD_FP32(sp_col_dense_index + 5*SIMD_WIDTH_FP32), sum[5]);
      }
      _MM_STORE_FP32(result_m_index + 0*SIMD_WIDTH_FP32, sum[0]);
      _MM_STORE_FP32(result_m_index + 1*SIMD_WIDTH_FP32, sum[1]);
      _MM_STORE_FP32(result_m_index + 2*SIMD_WIDTH_FP32, sum[2]);
      _MM_STORE_FP32(result_m_index + 3*SIMD_WIDTH_FP32, sum[3]);
      _MM_STORE_FP32(result_m_index + 4*SIMD_WIDTH_FP32, sum[4]);
      _MM_STORE_FP32(result_m_index + 5*SIMD_WIDTH_FP32, sum[5]);
    }
    else {
      int64_t j = 0;
      for (n = 0; n < num_full_regs; n += 2) {
        sum[n] = _MM_SETZERO_FP32();
        sum[n+1] = _MM_SETZERO_FP32();
      }
      for (; j < num_j; j++) {
        const float *const LIBXSMM_RESTRICT sp_col_dense_index = scratch_B_base + (size_t)sp_c_ptr_base[j]*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32;
        SIMDTYPE_FP32 v_v = _MM_SET1_FP32(sp_v_ptr_base[j]);
        for (n = 0; n < num_full_regs; n += 2) {
          sum[n]   = _MM_FMADD_FP32(v_v, _MM_LOAD_FP32(sp_col_dense_index + ((size_t)n)  *SIMD_WIDTH_FP32), sum[n]);
          sum[n+1] = _MM_FMADD_FP32(v_v, _MM_LOAD_FP32(sp_col_dense_index + ((size_t)n+1)*SIMD_WIDTH_FP32), sum[n+1]);
        }
        {
          float v_v_f = sp_v_ptr_base[j];
          for (n = last_n_start; n < num_n; n++) {
            result_m_index[n] += sp_col_dense_index[n]*v_v_f;
          }
        }
      }
      for (n = 0; n < num_full_regs; n += 2) {
        _MM_STORE_FP32(result_m_index + ((size_t)n)  *SIMD_WIDTH_FP32, _MM_ADD_FP32(sum[n],   _MM_LOAD_FP32(result_m_index + ((size_t)n)  *SIMD_WIDTH_FP32)));
        _MM_STORE_FP32(result_m_index + ((size_t)n+1)*SIMD_WIDTH_FP32, _MM_ADD_FP32(sum[n+1], _MM_LOAD_FP32(result_m_index + ((size_t)n+1)*SIMD_WIDTH_FP32)));
      }

    }
  }
} /* kb */

/* Copy out c matrix */
if ('T' == transc || 't' == transc) {
  int num_m_simd = num_m / SIMD_WIDTH_FP32 * SIMD_WIDTH_FP32;
  int num_n_simd = num_n / SIMD_WIDTH_FP32 * SIMD_WIDTH_FP32;
  int n2;

  ptr_result = c + (size_t)n_overall_start*handle->m + m_overall_start;
  for (n = 0; n < num_n_simd; n += SIMD_WIDTH_FP32) {
    for (m = 0; m < num_m_simd; m += SIMD_WIDTH_FP32) {
      TRANSPOSE_SIMD_WIDTH_KERNEL(scratch_C + (size_t)m*n_block_size + n, n_block_size, ptr_result + (size_t)n*handle->m + m, handle->m);
    }
    /* Transpose a SIMD_WIDTH_FP32 * (num_m - num_m_simd) block of output space - input is of size (num_m - num_m_simd) * SIMD_WIDTH_FP32 */
    for (n2 = n; n2 < n + SIMD_WIDTH_FP32; n2++) {
      for (m = num_m_simd; m < num_m; m++) {
        ptr_result[n2*handle->m + m] = scratch_C[m*n_block_size + n2];
      }
    }
  }
  /* Transpose a (num_n - num_n_simd) * num_m block of output space - input is of size num_m * (num_n - num_n_simd) */
  for (n = num_n_simd; n < num_n; n++) {
    for (m = 0; m < num_m; m++) {
      ptr_result[n*handle->m + m] = scratch_C[m*n_block_size + n];
    }
  }
}
else {
  if (!last_block_n) {
    for (m = 0; m < num_m; m++) {
      _MM_STOREU_FP32(ptr_result + (size_t)m*handle->n + 0*SIMD_WIDTH_FP32, _MM_LOAD_FP32(scratch_C + (size_t)m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + 0*SIMD_WIDTH_FP32));
      _MM_STOREU_FP32(ptr_result + (size_t)m*handle->n + 1*SIMD_WIDTH_FP32, _MM_LOAD_FP32(scratch_C + (size_t)m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + 1*SIMD_WIDTH_FP32));
      _MM_STOREU_FP32(ptr_result + (size_t)m*handle->n + 2*SIMD_WIDTH_FP32, _MM_LOAD_FP32(scratch_C + (size_t)m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + 2*SIMD_WIDTH_FP32));
      _MM_STOREU_FP32(ptr_result + (size_t)m*handle->n + 3*SIMD_WIDTH_FP32, _MM_LOAD_FP32(scratch_C + (size_t)m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + 3*SIMD_WIDTH_FP32));
      _MM_STOREU_FP32(ptr_result + (size_t)m*handle->n + 4*SIMD_WIDTH_FP32, _MM_LOAD_FP32(scratch_C + (size_t)m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + 4*SIMD_WIDTH_FP32));
      _MM_STOREU_FP32(ptr_result + (size_t)m*handle->n + 5*SIMD_WIDTH_FP32, _MM_LOAD_FP32(scratch_C + (size_t)m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + 5*SIMD_WIDTH_FP32));
    }
  }
  else {
    for (m = 0; m < num_m; m++) {
      for (n = 0; n < num_full_regs; n += 2) {
        _MM_STOREU_FP32(ptr_result + (size_t)m*handle->n + ((size_t)n)  *SIMD_WIDTH_FP32, _MM_LOAD_FP32(scratch_C + (size_t)m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + ((size_t)n)  *SIMD_WIDTH_FP32));
        _MM_STOREU_FP32(ptr_result + (size_t)m*handle->n + ((size_t)n+1)*SIMD_WIDTH_FP32, _MM_LOAD_FP32(scratch_C + (size_t)m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + ((size_t)n+1)*SIMD_WIDTH_FP32));
      }
      for (n = last_n_start; n < num_n; n++) {
        ptr_result[m*handle->n + n] = scratch_C[m*LIBXSMM_SPMDM_COMPUTE_NREGS*SIMD_WIDTH_FP32 + n];
      }
    }
  }
}

#undef LIBXSMM_SPMDM_COMPUTE_NREGS
