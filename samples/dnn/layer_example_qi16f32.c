/******************************************************************************
** Copyright (c) 2017-2018, Intel Corporation                                **
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
/* Alexander Heinecke, Hans Pabst, Dhiraj Kalamkar,
   Rajkishore Barik (Intel Corp.)
******************************************************************************/
#include <libxsmm.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#if defined(_OPENMP)
# include <omp.h>
#endif

# define USE_OVERWRITE
/*# define USE_BWD_NO_FILTER_TRANSPOSE_OVERWRITE*/
/*# define USE_FUSED_BATCH_STATS*/
/*# define USE_FUSED_MAX_STATS*/
#define FP64_BN_STATS
/*#define USE_FUSED_RELU_BWD*/
#define USE_FORMATED_QUANT

#if !defined(USE_FUSED_BIAS) && 0
# define USE_FUSED_BIAS
#endif
#if !defined(USE_FUSED_RELU) && 0
# define USE_FUSED_RELU
#endif

#if !defined(USE_FUSED) && 0
# define USE_FUSED_BIAS_RELU
#endif

#if defined(_WIN32) || defined(__CYGWIN__) || !(defined(_SVID_SOURCE) || defined(_XOPEN_SOURCE))
# define drand48() ((double)rand() / RAND_MAX)
# define srand48 srand
#endif

#define CHKERR_LIBXSMM_DNN(A) if ( A != LIBXSMM_DNN_SUCCESS ) { fprintf(stderr, "%s\n", libxsmm_dnn_get_error(A) ); global_status = A; }

typedef struct {
  int nImg;
  int nIfm;
  int nOfm;
  int ifhp;
  int ifwp;
  int ifh;
  int ifw;
  int ofhp;
  int ofwp;
  int ofh;
  int ofw;
  int pad_h;
  int pad_w;
  int pad_h_in;
  int pad_w_in;
  int pad_h_out;
  int pad_w_out;
  int kh;
  int kw;
  int stride_h;
  int stride_w;
} naive_conv_t;

LIBXSMM_INLINE void zero_buf(float* buf, size_t size) {
  int i;
#pragma omp parallel for private(i)
  for (i = 0; i < size; ++i) {
    buf[i] = 0.0f;
  }
}

LIBXSMM_INLINE void zero_buf_i16(short* buf, size_t size) {
  int i;
#pragma omp parallel for private(i)
  for (i = 0; i < size; ++i) {
    buf[i] = 0;
  }
}


LIBXSMM_INLINE void copy_buf(float* src, float* dst, size_t size) {
  int i;
#pragma omp parallel for private(i)
  for (i = 0; i < size; ++i) {
    dst[i] = src[i];
  }
}

LIBXSMM_INLINE void init_buf(float* buf, size_t size, int initPos, int initOne)
{
  int i;
  zero_buf(buf, size);
  for (i = 0; i < size; ++i) {
    buf[i] = (float)((initOne != 0) ? 1.0 : ((initPos != 0) ? drand48() : (0.05 - drand48()/10.0)));
  }
}

LIBXSMM_INLINE void set_zeropad_nchw(float* nchw, int N, int C, int H, int W, int pad_h, int pad_w)
{
  LIBXSMM_VLA_DECL(4, float, input, nchw, C, H, W);
  int n, h, w, c;

  for ( n = 0; n < N; n++ ) {
    for ( c = 0; c < C; c++ ) {
      for ( h = 0; h < H; h++ ) {
        for ( w = 0; w < W; w++ ) {
          if (h < pad_h || h >= H-pad_h || w < pad_w || w >= W-pad_w)
            LIBXSMM_VLA_ACCESS(4,  input, n, c, h, w, C, H, W) = 0.0;
        }
      }
    }
  }
}

LIBXSMM_INLINE void copy_internal_nchw(float* dst , float* src, int N, int C, int H, int W, int pad_h, int pad_w)
{
  LIBXSMM_VLA_DECL(4, float, input, src, C, H, W);
  LIBXSMM_VLA_DECL(4, float, new_input, dst, C, H+2*pad_h, W+2*pad_w);
  int n, h, w, c;

  for ( n = 0; n < N; n++ ) {
    for ( c = 0; c < C; c++ ) {
      for ( h = 0; h < H; h++ ) {
        for ( w = 0; w < W; w++ ) {
          LIBXSMM_VLA_ACCESS(4, new_input, n, c, h+pad_h, w+pad_w, C, H+2*pad_h, W+2*pad_w) =  LIBXSMM_VLA_ACCESS(4,  input, n, c, h, w, C, H, W);
        }
      }
    }
  }
}

LIBXSMM_INLINE void naive_copy_NCHW_to_NHWC(const float* nchw, float* nhwc, int N, int H, int W, int C)
{
  LIBXSMM_VLA_DECL(4,       float, output, nhwc, H, W, C);
  LIBXSMM_VLA_DECL(4, const float,  input, nchw, C, H, W);
  int n, h, w, c;

  for ( n = 0; n < N; n++ ) {
    for ( h = 0; h < H; h++ ) {
      for ( w = 0; w < W; w++ ) {
        for ( c = 0; c < C; c++ ) {
          LIBXSMM_VLA_ACCESS(4, output, n, h, w, c, H, W, C) =
          LIBXSMM_VLA_ACCESS(4,  input, n, c, h, w, C, H, W);
        }
      }
    }
  }
}


LIBXSMM_INLINE void naive_copy_NHWC_to_NCHW(const float* nhwc, float* nchw, int N, int H, int W, int C)
{
  LIBXSMM_VLA_DECL(4,       float, output, nchw, C, H, W);
  LIBXSMM_VLA_DECL(4, const float,  input, nhwc, H, W, C);
  int n, h, w, c;

  for ( n = 0; n < N; n++ ) {
    for ( h = 0; h < H; h++ ) {
      for ( w = 0; w < W; w++ ) {
        for ( c = 0; c < C; c++ ) {
          LIBXSMM_VLA_ACCESS(4, output, n, c, h, w, C, H, W) =
          LIBXSMM_VLA_ACCESS(4,  input, n, h, w, c, H, W, C);
        }
      }
    }
  }
}


LIBXSMM_INLINE void naive_copy_KCRS_to_RSCK(const float* kcrs, float* rsck, int R, int S, int C, int K)
{
  LIBXSMM_VLA_DECL(4,       float, output, rsck, S, C, K);
  LIBXSMM_VLA_DECL(4, const float,  input, kcrs, C, R, S);
  int r, s, c, k;

  for ( r = 0; r < R; r++ ) {
    for ( s = 0; s < S; s++ ) {
      for ( c = 0; c < C; c++ ) {
        for ( k = 0; k < K; k++ ) {
          LIBXSMM_VLA_ACCESS(4, output, r, s, c, k, S, C, K) =
          LIBXSMM_VLA_ACCESS(4,  input, k, c, r, s, C, R, S);
        }
      }
    }
  }
}


LIBXSMM_INLINE void naive_copy_RSCK_to_KCRS(const float* rsck, float* kcrs, int R, int S, int C, int K)
{
  LIBXSMM_VLA_DECL(4, const float,  input, rsck, S, C, K);
  LIBXSMM_VLA_DECL(4,       float, output, kcrs, C, R, S);
  int r, s, c, k;

  for ( r = 0; r < R; r++ ) {
    for ( s = 0; s < S; s++ ) {
      for ( c = 0; c < C; c++ ) {
        for ( k = 0; k < K; k++ ) {
          LIBXSMM_VLA_ACCESS(4, output, k, c, r, s, C, R, S) =
            LIBXSMM_VLA_ACCESS(4,  input, r, s, c, k, S, C, K);
        }
      }
    }
  }
}


LIBXSMM_INLINE void naive_conv_fp(naive_conv_t* param, const float* input, float* output, const float* filter, const float* bias)
{
  int nImg      = param->nImg;
  int nIfm      = param->nIfm;
  int nOfm      = param->nOfm;
  int ifhp      = param->ifhp;
  int ifwp      = param->ifwp;
  int ofhp      = param->ofhp;
  int ofwp      = param->ofwp;
  int ifh       = param->ifh;
  int ifw       = param->ifw;
  int ofh       = param->ofh;
  int ofw       = param->ofw;
  int pad_h     = param->pad_h;
  int pad_w     = param->pad_w;
  int pad_h_in  = param->pad_h_in;
  int pad_w_in  = param->pad_w_in;
  int pad_h_out = param->pad_h_out;
  int pad_w_out = param->pad_w_out;
  int kh        = param->kh;
  int kw        = param->kw;
  int stride_h  = param->stride_h;
  int stride_w  = param->stride_w;
  /* loop counters */
  int img, ofm, ifm, oj, oi, ij, ii, kj, ki;

  LIBXSMM_VLA_DECL(4,       float, output_t, output + (pad_w_out * ofwp + pad_h_out), nOfm, ofhp, ofwp);
  LIBXSMM_VLA_DECL(4, const float,  input_t,  input + (pad_w_in * ifwp + pad_h_in), nIfm, ifhp, ifwp);
  LIBXSMM_VLA_DECL(4, const float, filter_t, filter, nIfm, kh, kw);

#if defined(USE_FUSED_BIAS) || defined(USE_FUSED_BIAS_RELU)
#if defined(_OPENMP)
# pragma omp parallel for LIBXSMM_OPENMP_COLLAPSE(2) private(img, ofm, ifm, oj, oi, ij, ii, kj, ki)
#endif
  for (img = 0; img < nImg; ++img) {
    for (ofm = 0; ofm < nOfm; ++ofm) {
      for (oj = 0; oj < ofh; ++oj) {
        for (oi = 0; oi < ofw; ++oi) {
          LIBXSMM_VLA_ACCESS(  4, output_t, img, ofm, oj, oi, nOfm, ofhp, ofwp) = bias[ofm];
        }
      }
    }
  }
#endif

#if defined(_OPENMP)
# pragma omp parallel for LIBXSMM_OPENMP_COLLAPSE(2) private(img, ofm, ifm, oj, oi, ij, ii, kj, ki)
#endif
  for (img = 0; img < nImg; ++img) {
    for (ofm = 0; ofm < nOfm; ++ofm) {
      for (ifm = 0; ifm < nIfm; ++ifm) {
        for (oj = 0; oj < ofh; ++oj) {
          ij = oj * stride_h - pad_h;
          for (oi = 0; oi < ofw; ++oi) {
            ii = oi * stride_w - pad_w;
            for (kj = 0; kj < kh; ++kj) {
              if (ij+kj < 0 || ij+kj >= ifh) continue;
              for (ki = 0; ki < kw; ++ki) {
                if (ii+ki < 0 || ii+ki >= ifw) continue;
                LIBXSMM_VLA_ACCESS(  4, output_t, img, ofm, oj, oi, nOfm, ofhp, ofwp) +=
                  LIBXSMM_VLA_ACCESS(4,  input_t, img, ifm, ij + kj, ii + ki, nIfm, ifhp, ifwp)
                * LIBXSMM_VLA_ACCESS(4, filter_t, ofm, ifm, kj, ki, nIfm, kh, kw);
              }
            }
          }
        }
      }
#if defined(USE_FUSED_RELU) || defined(USE_FUSED_BIAS_RELU)
      for (oj = 0; oj < ofh; ++oj) {
        for (oi = 0; oi < ofw; ++oi) {
          LIBXSMM_VLA_ACCESS(  4, output_t, img, ofm, oj, oi, nOfm, ofhp, ofwp) =
           (LIBXSMM_VLA_ACCESS(  4, output_t, img, ofm, oj, oi, nOfm, ofhp, ofwp) < 0.0f) ? 0.0f : LIBXSMM_VLA_ACCESS(  4, output_t, img, ofm, oj, oi, nOfm, ofhp, ofwp);
        }
      }
#endif
    }
  }
}

LIBXSMM_INLINE void naive_conv_bp(naive_conv_t* param, float* input, const float* output, const float* filter, const float* naive_input_save)
{
  int nImg      = param->nImg;
  int nIfm      = param->nIfm;
  int nOfm      = param->nOfm;
  int ifhp      = param->ifhp;
  int ifwp      = param->ifwp;
  int ofhp      = param->ofhp;
  int ofwp      = param->ofwp;
  int ifh       = param->ifh;
  int ifw       = param->ifw;
  int ofh       = param->ofh;
  int ofw       = param->ofw;
  int pad_h     = param->pad_h;
  int pad_w     = param->pad_w;
  int pad_h_in  = param->pad_h_in;
  int pad_w_in  = param->pad_w_in;
  int pad_h_out = param->pad_h_out;
  int pad_w_out = param->pad_w_out;
  int kh        = param->kh;
  int kw        = param->kw;
  int stride_h  = param->stride_h;
  int stride_w  = param->stride_w;
  /* loop counters */
  int img, ofm, ifm, oj, oi, ij, ii, kj, ki;

  LIBXSMM_VLA_DECL(4, const float, output_t, output + (pad_w_out * ofwp + pad_h_out), nOfm, ofhp, ofwp);
  LIBXSMM_VLA_DECL(4,       float,  input_t,  input + (pad_w_in * ifwp + pad_h_in), nIfm, ifhp, ifwp);
  LIBXSMM_VLA_DECL(4, const float, filter_t, filter, nIfm, kh, kw);
#if defined(USE_FUSED_RELU_BWD)
  LIBXSMM_VLA_DECL(4, const float, naive_input_t, naive_input_save + (pad_w_in * ifwp + pad_h_in), nIfm, ifhp, ifwp);
#endif

#if defined(_OPENMP)
# pragma omp parallel for LIBXSMM_OPENMP_COLLAPSE(2) private(img, ofm, ifm, oj, oi, ij, ii, kj, ki)
#endif
  for (img = 0; img < nImg; ++img) {
    for (ifm = 0; ifm < nIfm; ++ifm) {
      for (ofm = 0; ofm < nOfm; ++ofm) {
        for (oj = 0; oj < ofh; ++oj) {
          ij = oj * stride_h - pad_h;
          for (oi = 0; oi < ofw; ++oi) {
            ii = oi * stride_w - pad_w;
            for (kj = 0; kj < kh; ++kj) {
              if (ij+kj < 0 || ij+kj >= ifh) continue;
              for (ki = 0; ki < kw; ++ki) {
                if (ii+ki < 0 || ii+ki >= ifw) continue;
                LIBXSMM_VLA_ACCESS(4,  input_t, img, ifm, ij + kj, ii + ki, nIfm, ifhp, ifwp) +=
                  LIBXSMM_VLA_ACCESS(4, output_t, img, ofm, oj, oi, nOfm, ofhp, ofwp)
                * LIBXSMM_VLA_ACCESS(4, filter_t, ofm, ifm, kj, ki, nIfm, kh, kw);
              }
            }
          }
        }
      }
#if defined(USE_FUSED_RELU_BWD)
      for (ij = 0; ij < ifh; ij++) {
        for (ii = 0; ii < ifw; ii++) {
          if ( LIBXSMM_VLA_ACCESS(4,  naive_input_t, img, ifm, ij, ii , nIfm, ifhp, ifwp) == 0.0 ) {
            LIBXSMM_VLA_ACCESS(4, input_t, img, ifm, ij, ii , nIfm, ifhp, ifwp) = 0.0;
          }
        }
      }
#endif
    }
  }
}

LIBXSMM_INLINE void naive_conv_wu(naive_conv_t* param, const float* input, const float* output, float* filter)
{
  int nImg      = param->nImg;
  int nIfm      = param->nIfm;
  int nOfm      = param->nOfm;
  int ifhp      = param->ifhp;
  int ifwp      = param->ifwp;
  int ofhp      = param->ofhp;
  int ofwp      = param->ofwp;
  int ifh       = param->ifh;
  int ifw       = param->ifw;
  int ofh       = param->ofh;
  int ofw       = param->ofw;
  int pad_h     = param->pad_h;
  int pad_w     = param->pad_w;
  int pad_h_in  = param->pad_h_in;
  int pad_w_in  = param->pad_w_in;
  int pad_h_out = param->pad_h_out;
  int pad_w_out = param->pad_w_out;
  int kh        = param->kh;
  int kw        = param->kw;
  int stride_h  = param->stride_h;
  int stride_w  = param->stride_w;
  /* loop counters */
  int img, ofm, ifm, oj, oi, ij, ii, kj, ki;

  LIBXSMM_VLA_DECL(4, const float, output_t, output + (pad_w_out * ofwp + pad_h_out), nOfm, ofhp, ofwp);
  LIBXSMM_VLA_DECL(4, const float,  input_t,  input + (pad_w_in * ifwp + pad_h_in), nIfm, ifhp, ifwp);
  LIBXSMM_VLA_DECL(4,       float, filter_t, filter, nIfm, kh, kw);

#if defined(_OPENMP)
# pragma omp parallel for LIBXSMM_OPENMP_COLLAPSE(2) private(img, ofm, ifm, oj, oi, ij, ii, kj, ki)
#endif
  for (ofm = 0; ofm < nOfm; ++ofm) {
    for (ifm = 0; ifm < nIfm; ++ifm) {
      for (img = 0; img < nImg; ++img) {
        for (oj = 0; oj < ofh; ++oj) {
          ij = oj * stride_h - pad_h;
          for (oi = 0; oi < ofw; ++oi) {
            ii = oi * stride_w - pad_w;
            for (kj = 0; kj < kh; ++kj) {
              if (ij+kj < 0 || ij+kj >= ifh) continue;
              for (ki = 0; ki < kw; ++ki) {
                if (ii+ki < 0 || ii+ki >= ifw) continue;
                LIBXSMM_VLA_ACCESS(4, filter_t, ofm, ifm, kj, ki, nIfm, kh, kw) +=
                  LIBXSMM_VLA_ACCESS(4,  input_t, img, ifm, ij + kj, ii + ki, nIfm, ifhp, ifwp)
                * LIBXSMM_VLA_ACCESS(4, output_t, img, ofm, oj, oi, nOfm, ofhp, ofwp);
              }
            }
          }
        }
      }
    }
  }
}

int main(int argc, char* argv[])
{
  float *naive_input, *naive_output, *naive_output_save, *naive_filter, *naive_filter_wu, *naive_output_bp, *naive_output_wu, *naive_libxsmm_output;
  float *naive_libxsmm_input, *naive_libxsmm_filter, *naive_input_save, *naive_filter_save;
  float *naive_bias, *naive_dbias, *dbias_libxsmm;
  float *output_libxsmm, *dinput_libxsmm, *dfilter_libxsmm, *bias_libxsmm;
  short *input_libxsmm, *filter_libxsmm, *doutput_libxsmm, *filtertr_libxsmm;
  short *i16_naive_input, *i16_naive_filter, *i16_naive_doutput;
  float *dq_naive_input, *dq_naive_filter, *dq_naive_doutput;
  unsigned char scf_input, scf_filter, scf_doutput/*, scf_filtertr*/;

#ifdef FP32_BN_STATS
  float *batchstats_libxsmm;
#endif
#ifdef FP64_BN_STATS
  double *batchstats_libxsmm;
#endif
#ifdef USE_FUSED_MAX_STATS
  float *maxstats_libxsmm_fwd;
  float *maxstats_libxsmm_bwd;
  float *maxstats_libxsmm_upd;
#endif

  int ifhp, ifwp, ofhp, ofwp, ofh, ofw;
  int stride_h, stride_w, pad_h, pad_w, pad_h_in, pad_w_in, pad_h_out, pad_w_out;
  naive_conv_t naive_param;
  void* scratch;
  size_t scratch_size = 0;

  /* some parameters we can overwrite via cli,
     default is some inner layer of overfeat */
  int iters = 10;         /* repetitions of benchmark */
  int ifw = 14;           /* input width, "W" */
  int ifh = 20;           /* input height, "H" */
  int nImg = 32;          /* mini-batch size, "N" */
  int nIfm = 256;         /* number of input feature maps, "C" */
  int nOfm = 512;         /* number of output feature maps, "K" */
  int kh = 3;             /* filter height, "R" */
  int kw = 3;             /* filter width, "S" */
  int padh = 0;           /* padding in input, height */
  int padw = 0;           /* padding in input, width */
  int stride = 1;         /* stride when accessing inputs */
  int padding_mode = 0;   /* padding mode */
  char type = 'A';        /* 'A': ALL, 'F': FP, 'B': BP, 'U', WU */
  char format = 'L';

  const char *const env_check = getenv("CHECK"), *const env_winograd = getenv("WINOGRAD");
  const double check = LIBXSMM_ABS(0 == env_check ? 1 : atof(env_check));
  const int algo_winograd = (0 == env_winograd ? 0 : atoi(env_winograd));

  #if defined(_OPENMP)
  int nThreads = omp_get_max_threads();      /* number of threads */
#else
  int nThreads = 1;       /* number of threads */
#endif

  unsigned long long l_start, l_end;
  double l_total = 0.0;
  double flops = 0.0;
  int i;

  libxsmm_dnn_conv_desc conv_desc;
  libxsmm_dnn_layer* libxsmm_handle;
  libxsmm_dnn_tensor* libxsmm_input;
  libxsmm_dnn_tensor* libxsmm_output;
  libxsmm_dnn_tensor* libxsmm_filter;
  libxsmm_dnn_tensor* libxsmm_dinput;
  libxsmm_dnn_tensor* libxsmm_doutput;
  libxsmm_dnn_tensor* libxsmm_dfilter;
  libxsmm_dnn_tensor* libxsmm_filter_tr;
  libxsmm_dnn_tensor* libxsmm_bias;
  libxsmm_dnn_tensor* libxsmm_dbias;
#ifdef USE_FUSED_MAX_STATS
  libxsmm_dnn_tensor* libxsmm_batchstats;
  libxsmm_dnn_tensor* libxsmm_maxstats_fwd;
  libxsmm_dnn_tensor* libxsmm_maxstats_bwd;
  libxsmm_dnn_tensor* libxsmm_maxstats_upd;
#endif
  libxsmm_dnn_tensor_datalayout* libxsmm_layout;
  libxsmm_dnn_err_t status;
  libxsmm_dnn_err_t global_status = LIBXSMM_DNN_SUCCESS;

  libxsmm_matdiff_info norms_fwd, norms_bwd, norms_upd, diff, norms_batchstats, norms_quant;
  memset(&norms_fwd, 0, sizeof(norms_fwd));
  memset(&norms_bwd, 0, sizeof(norms_bwd));
  memset(&norms_upd, 0, sizeof(norms_upd));
  memset(&norms_batchstats, 0, sizeof(norms_batchstats));
  memset(&diff, 0, sizeof(diff));
  memset(&norms_quant, 0, sizeof(norms_quant));

  if (argc > 1 && !strncmp(argv[1], "-h", 3)) {
    printf("Usage: %s iters inpWidth inpHeight nImg nIfm nOfm kw kh pad stride type format padding_mode\n", argv[0]);
    return 0;
  }
  srand48(1);

  /* reading new values from cli */
  i = 1;
  if (argc > i) iters      = atoi(argv[i++]);
  if (argc > i) ifw        = atoi(argv[i++]);
  if (argc > i) ifh        = atoi(argv[i++]);
  if (argc > i) nImg       = atoi(argv[i++]);
  if (argc > i) nIfm       = atoi(argv[i++]);
  if (argc > i) nOfm       = atoi(argv[i++]);
  if (argc > i) kw         = atoi(argv[i++]);
  if (argc > i) kh         = atoi(argv[i++]);
  if (argc > i) padw       = atoi(argv[i++]);
  if (argc > i) padh       = atoi(argv[i++]);
  if (argc > i) stride     = atoi(argv[i++]);
  if (argc > i) type       = *(argv[i++]);
  if (argc > i) format     = *(argv[i++]);
  if (argc > i) padding_mode = atoi(argv[i++]);

  if (type != 'A' && type != 'F' && type != 'B' && type != 'U') {
    printf("type needs to be 'A' (All), 'F' (FP only), 'B' (BP only), 'U' (WU only)\n");
    return 0;
  }

  stride_w = stride;
  stride_h = stride;
  pad_w = padw;
  pad_h = padh;

    pad_h_in = 0;
    pad_w_in = 0;
    pad_h_out = 0;
    pad_w_out = 0;

  if (0 == padding_mode) {
    pad_h_in = 0;
    pad_w_in = 0;
    pad_h_out = 0;
    pad_w_out = 0;
  }
  else {
    /* TODO: change "1" to "0" if "padding_mode = -1" is acknowledged */
    if (1 < padding_mode) pad_w = padding_mode;
    pad_h_in = pad_h;
    pad_w_in = pad_w;
    pad_h_out = pad_h;
    pad_w_out = pad_w;
  }

  /* deriving some values for naive code */
  ofh = (ifh + 2 * pad_h - kh) / stride_h + 1;
  ofw = (ifw + 2 * pad_w - kw) / stride_w + 1;
  ifhp = ifh + 2 * pad_h_in;
  ifwp = ifw + 2 * pad_w_in;
  ofhp = ofh + 2 * pad_h_out;
  ofwp = ofw + 2 * pad_w_out;

  /* set struct for naive convolution */
  naive_param.nImg = nImg;
  naive_param.nIfm = nIfm;
  naive_param.nOfm = nOfm;
  naive_param.ifhp = ifhp;
  naive_param.ifwp = ifwp;
  naive_param.ofhp = ofhp;
  naive_param.ofwp = ofwp;
  naive_param.ifh = ifh;
  naive_param.ifw = ifw;
  naive_param.ofh = ofh;
  naive_param.ofw = ofw;
  naive_param.pad_h = pad_h;
  naive_param.pad_w = pad_w;
  naive_param.pad_h_in = pad_h_in;
  naive_param.pad_w_in = pad_w_in;
  naive_param.pad_h_out = pad_h_out;
  naive_param.pad_w_out = pad_w_out;
  naive_param.kh = kh;
  naive_param.kw = kw;
  naive_param.stride_h = stride_h;
  naive_param.stride_w = stride_w;

  /* print some summary */
  printf("##########################################\n");
  printf("#          Setting Up (Common)           #\n");
  printf("##########################################\n");
  printf("PARAMS: W:%d  H:%d  N:%d  C:%d  K:%d  R:%d  S:%d  P:%d  Q:%d  STRIDE:%d\n", ifw, ifh, nImg, nIfm, nOfm, kw, kh, ofh, ofw, stride);
  printf("PARAMS: ITERS:%d", iters); if (LIBXSMM_FEQ(0, check)) printf("  Threads:%d\n", nThreads); else printf("\n");
  printf(" InImg %dx%d Padded (%dx%d)\n", ifh, ifw, ifhp, ifwp);
  printf("OutImg %dx%d Padded (%dx%d)\n", ofh, ofw, ofhp, ofwp);
  printf("SIZE Input  (MB): %10.2f MiB\n", (double)(nImg*nIfm*ifhp*ifwp*sizeof(float))/(1024.0*1024.0) );
  printf("SIZE Output (MB): %10.2f MiB\n", (double)(nImg*nOfm*ofhp*ofwp*sizeof(float))/(1024.0*1024.0) );
  printf("SIZE Input   (1): %10.2f MiB\n", (double)(1*nIfm*ifhp*ifwp*   sizeof(float))/(1024.0*1024.0) );
  printf("SIZE Output  (1): %10.2f MiB\n", (double)(1*nOfm*ofhp*ofwp*   sizeof(float))/(1024.0*1024.0) );
  printf("SIZE Weight     : %10.2f MiB\n", (double)(nIfm*nOfm*kw*kh*    sizeof(float))/(1024.0*1024.0) );
#if defined(USE_OVERWRITE)
  printf("Using Overwrite Option\n");
#endif

  /* allocate data */
  naive_input           = (float*)libxsmm_aligned_malloc( nImg*nIfm*ifhp*ifwp*sizeof(float), 2097152);
  naive_input_save      = (float*)libxsmm_aligned_malloc( nImg*nIfm*ifhp*ifwp*sizeof(float), 2097152);
  naive_output          = (float*)libxsmm_aligned_malloc( nImg*nOfm*ofhp*ofwp*sizeof(float), 2097152);
  naive_output_save     = (float*)libxsmm_aligned_malloc( nImg*nOfm*ofhp*ofwp*sizeof(float), 2097152);
  naive_output_bp       = (float*)libxsmm_aligned_malloc( nImg*nOfm*ofhp*ofwp*sizeof(float), 2097152);
  naive_output_wu       = (float*)libxsmm_aligned_malloc( nImg*nOfm*ofhp*ofwp*sizeof(float), 2097152);
  naive_libxsmm_output  = (float*)libxsmm_aligned_malloc( nImg*nOfm*ofhp*ofwp*sizeof(float), 2097152);
  naive_libxsmm_input   = (float*)libxsmm_aligned_malloc( nImg*nIfm*ifhp*ifwp*sizeof(float), 2097152);
  naive_filter          = (float*)libxsmm_aligned_malloc( nOfm*nIfm*kh*kw*    sizeof(float), 2097152);
  naive_filter_save     = (float*)libxsmm_aligned_malloc( nOfm*nIfm*kh*kw*    sizeof(float), 2097152);
  naive_filter_wu       = (float*)libxsmm_aligned_malloc( nOfm*nIfm*kh*kw*    sizeof(float), 2097152);
  naive_libxsmm_filter  = (float*)libxsmm_aligned_malloc( nOfm*nIfm*kh*kw*    sizeof(float), 2097152);
  input_libxsmm         = (short*)libxsmm_aligned_malloc( nImg*nIfm*ifhp*ifwp*sizeof(short), 2097152);
  filter_libxsmm        = (short*)libxsmm_aligned_malloc( nOfm*nIfm*kh*kw*    sizeof(short), 2097152);
  output_libxsmm        = (float*)libxsmm_aligned_malloc( nImg*nOfm*ofhp*ofwp*sizeof(float), 2097152);
  dinput_libxsmm        = (float*)libxsmm_aligned_malloc( nImg*nIfm*ifhp*ifwp*sizeof(float), 2097152);
  dfilter_libxsmm       = (float*)libxsmm_aligned_malloc( nOfm*nIfm*kh*kw*    sizeof(float), 2097152);
  doutput_libxsmm       = (short*)libxsmm_aligned_malloc( nImg*nOfm*ofhp*ofwp*sizeof(short), 2097152);
  filtertr_libxsmm      = (short*)libxsmm_aligned_malloc( nOfm*nIfm*kh*kw*    sizeof(short), 2097152);
  i16_naive_input       = (short*)libxsmm_aligned_malloc( nImg*nIfm*ifhp*ifwp*sizeof(short), 2097152);
  i16_naive_filter      = (short*)libxsmm_aligned_malloc( nOfm*nIfm*kh*kw*    sizeof(short), 2097152);
  i16_naive_doutput     = (short*)libxsmm_aligned_malloc( nImg*nOfm*ofhp*ofwp*sizeof(short), 2097152);
  dq_naive_input        = (float*)libxsmm_aligned_malloc( nImg*nIfm*ifhp*ifwp*sizeof(float), 2097152);
  dq_naive_filter       = (float*)libxsmm_aligned_malloc( nOfm*nIfm*kh*kw*    sizeof(float), 2097152);
  dq_naive_doutput      = (float*)libxsmm_aligned_malloc( nImg*nOfm*ofhp*ofwp*sizeof(float), 2097152);
#ifdef FP32_BN_STATS
  batchstats_libxsmm    = (float*)libxsmm_aligned_malloc( 2*nImg*nOfm*        sizeof(float), 2097152);
#endif
#ifdef FP64_BN_STATS
  batchstats_libxsmm    = (double*)libxsmm_aligned_malloc( 2*nImg*nOfm*       sizeof(double), 2097152);
#endif
#ifdef USE_FUSED_MAX_STATS
  maxstats_libxsmm_fwd    = (float*)libxsmm_aligned_malloc(nImg*16*sizeof(float), 2097152);
  maxstats_libxsmm_bwd    = (float*)libxsmm_aligned_malloc(nImg*16*sizeof(float), 2097152);
  maxstats_libxsmm_upd    = (float*)libxsmm_aligned_malloc(nImg*16*sizeof(float), 2097152);
#endif
  naive_bias            = (float*)libxsmm_aligned_malloc( nOfm*               sizeof(float), 2097152);
  naive_dbias           = (float*)libxsmm_aligned_malloc( nOfm*               sizeof(float), 2097152);
  bias_libxsmm          = (float*)libxsmm_aligned_malloc( nOfm*               sizeof(float), 2097152);
  dbias_libxsmm         = (float*)libxsmm_aligned_malloc( nOfm*               sizeof(float), 2097152);

  /* initialize data */
  float *naive_input_tmp           = (float*)libxsmm_aligned_malloc( nImg*nIfm*ifhp*ifwp*sizeof(float), 2097152);
  if (padding_mode == 0 ) {
    init_buf(naive_input,          nImg*nIfm*ifhp*ifwp, 0, 0);
  } else {
    init_buf(naive_input_tmp,          nImg*nIfm*ifh*ifw, 0, 0);
    copy_internal_nchw( naive_input , naive_input_tmp, nImg, nIfm, ifh, ifw, pad_h, pad_w);
  }
#if defined(USE_FUSED_RELU_BWD)
  /* Initialize some entries with zeros  */
  {
    int i;
    for (i = 0; i < nImg*nIfm*ifhp*ifwp; i++ ) {
      if ( ((i%16) == 2) || ((i%16) == 3) || ((i%16) == 7) || ((i%16) == 14) ) {
        naive_input[i] = 0.0;
      }
    }
  }
#endif

  float *naive_output_bp_tmp       = (float*)libxsmm_aligned_malloc( nImg*nOfm*ofhp*ofwp*sizeof(float), 2097152);
  float *naive_output_wu_tmp       = (float*)libxsmm_aligned_malloc( nImg*nOfm*ofhp*ofwp*sizeof(float), 2097152);
  if (padding_mode == 0 ) {
    init_buf(naive_output_bp,      nImg*nOfm*ofhp*ofwp, 0, 0);
    init_buf(naive_output_wu,      nImg*nOfm*ofhp*ofwp, 0, 0);
  } else {
    init_buf(naive_output_bp_tmp,      nImg*nOfm*ofh*ofw, 0, 0);
    copy_internal_nchw( naive_output_bp , naive_output_bp_tmp, nImg, nOfm, ofh, ofw, pad_h, pad_w);
    init_buf(naive_output_wu_tmp,      nImg*nOfm*ofh*ofw, 0, 0);
    copy_internal_nchw( naive_output_wu , naive_output_wu_tmp, nImg, nOfm, ofh, ofw, pad_h, pad_w);
  }
  set_zeropad_nchw(naive_input, nImg, nIfm, ifhp, ifwp, pad_h_in, pad_w_in);
  set_zeropad_nchw(naive_output_bp, nImg, nOfm, ofhp, ofwp, pad_h_out, pad_w_out);
  set_zeropad_nchw(naive_output_wu, nImg, nOfm, ofhp, ofwp, pad_h_out, pad_w_out);

  copy_buf(naive_input, naive_input_save, nImg*nIfm*ifhp*ifwp);
  zero_buf(naive_output_save,    nImg*nOfm*ofhp*ofwp);

  float *naive_output_tmp          = (float*)libxsmm_aligned_malloc( nImg*nOfm*ofhp*ofwp*sizeof(float), 2097152);
  if (padding_mode == 0 ) {
    init_buf(naive_output,       nImg*nOfm*ofhp*ofwp, 0, 0);
  } else {
    init_buf(naive_output_tmp,       nImg*nOfm*ofh*ofw, 0, 0);
  }
  set_zeropad_nchw(naive_output, nImg, nOfm, ofhp, ofwp, pad_h_out, pad_w_out);

  copy_buf(naive_output, naive_output_save, nImg*nOfm*ofhp*ofwp);
  zero_buf(naive_libxsmm_output, nImg*nOfm*ofhp*ofwp);
  zero_buf(naive_libxsmm_input,  nImg*nIfm*ifhp*ifwp);
  init_buf(naive_filter,         nOfm*nIfm*kh*kw, 0, 0);
  copy_buf(naive_filter, naive_filter_wu, nOfm*nIfm*kh*kw);
  zero_buf(naive_libxsmm_filter, nOfm*nIfm*kh*kw);
  init_buf(naive_bias,           nOfm, 0, 0);
  init_buf(naive_dbias,          nOfm, 0, 0);

  /* first touch LIBXSMM */
  zero_buf_i16( input_libxsmm    , nImg*nIfm*ifhp*ifwp );
  zero_buf_i16( filter_libxsmm   , nOfm*nIfm*kh*kw );
  zero_buf( output_libxsmm   , nImg*nOfm*ofhp*ofwp );
  zero_buf( dinput_libxsmm   , nImg*nIfm*ifhp*ifwp );
  zero_buf( dfilter_libxsmm  , nOfm*nIfm*kh*kw );
  zero_buf_i16( doutput_libxsmm  , nImg*nOfm*ofhp*ofwp );
  zero_buf_i16( filtertr_libxsmm , nOfm*nIfm*kh*kw );

  if (LIBXSMM_NEQ(0, check)) {
    printf("##########################################\n");
    printf("#         Computing Reference ...        #\n");
    printf("##########################################\n");
    if (type == 'A' || type == 'F') {
#ifdef USE_OVERWRITE
      zero_buf(naive_output,    nImg*nOfm*ofhp*ofwp);
#endif
      naive_conv_fp(&naive_param, naive_input, naive_output, naive_filter, naive_bias);
    }
    if ( (type == 'A' || type == 'B') && (nIfm > 3) ) {
#ifdef USE_OVERWRITE
      zero_buf(naive_input,         nImg*nIfm*ifhp*ifwp);
#endif
      naive_conv_bp(&naive_param, naive_input, naive_output_bp, naive_filter, naive_input_save);
    }
    if (type == 'A' || type == 'U') {
      /* NB: We reuse naive_input_save for weight update because the input should not
       * have been modified between forward propagation and weight update; it further
       * helps in exploiting reuse to converted data. */
#ifdef USE_OVERWRITE
      zero_buf(naive_filter_wu,          nOfm*nIfm*kh*kw);
#endif
      naive_conv_wu(&naive_param, naive_input_save, naive_output_wu, naive_filter_wu);
    }
    printf("##########################################\n");
    printf("#      Computing Reference ... done      #\n");
    printf("##########################################\n");
  }

  if (format == 'A' || format == 'L') {
    printf("\n");
    printf("##########################################\n");
    printf("#      Setting Up  (custom-Storage)      #\n");
    printf("##########################################\n");

    /* setup LIBXSMM handle */
    conv_desc.N = nImg;
    conv_desc.C = nIfm;
    conv_desc.H = ifh;
    conv_desc.W = ifw;
    conv_desc.K = nOfm;
    conv_desc.R = kh;
    conv_desc.S = kw;
    conv_desc.u = stride_h;
    conv_desc.v = stride_w;
    conv_desc.pad_h = pad_h;
    conv_desc.pad_w = pad_w;
    conv_desc.pad_h_in = pad_h_in;
    conv_desc.pad_w_in = pad_w_in;
    conv_desc.pad_h_out = pad_h_out;
    conv_desc.pad_w_out = pad_w_out;
    conv_desc.threads = nThreads;
    conv_desc.algo = (0 == algo_winograd ? LIBXSMM_DNN_CONV_ALGO_DIRECT : LIBXSMM_DNN_CONV_ALGO_AUTO);
    conv_desc.buffer_format = LIBXSMM_DNN_TENSOR_FORMAT_LIBXSMM;
    conv_desc.filter_format = LIBXSMM_DNN_TENSOR_FORMAT_LIBXSMM;
    conv_desc.fuse_ops = LIBXSMM_DNN_CONV_FUSE_NONE;
#if defined(USE_BWD_NO_FILTER_TRANSPOSE_OVERWRITE)
    conv_desc.options = LIBXSMM_DNN_CONV_OPTION_BWD_NO_FILTER_TRANSPOSE_OVERWRITE;
#elif defined(USE_OVERWRITE)
    conv_desc.options = LIBXSMM_DNN_CONV_OPTION_OVERWRITE;
#else
    conv_desc.options = LIBXSMM_DNN_CONV_OPTION_NONE;
#endif
#if defined(USE_FUSED_BIAS)
    conv_desc.fuse_ops = LIBXSMM_DNN_CONV_FUSE_BIAS;
#elif defined(USE_FUSED_RELU)
    conv_desc.fuse_ops = LIBXSMM_DNN_CONV_FUSE_RELU;
#elif defined(USE_FUSED_BIAS_RELU)
    conv_desc.fuse_ops = LIBXSMM_DNN_CONV_FUSE_BIAS_RELU;
#elif (defined(USE_FUSED_BATCH_STATS) && defined(USE_FUSED_MAX_STATS))
    conv_desc.fuse_ops = LIBXSMM_DNN_CONV_FUSE_BATCH_STATS_AND_MAX;
#elif (defined(USE_FUSED_RELU_BWD) && defined(USE_FUSED_MAX_STATS))
    conv_desc.fuse_ops = LIBXSMM_DNN_CONV_FUSE_RELU_BWD_AND_MAX;
#elif defined(USE_FUSED_BATCH_STATS_RELU_BWD)
    conv_desc.fuse_ops = LIBXSMM_DNN_CONV_FUSE_BATCH_STATS_RELU_BWD;
#elif defined(USE_FUSED_BATCH_STATS_RELU_BWD_AND_MAX)
    conv_desc.fuse_ops = LIBXSMM_DNN_CONV_FUSE_BATCH_STATS_RELU_BWD_AND_MAX;
#elif defined(USE_FUSED_BATCH_STATS)
    conv_desc.fuse_ops = LIBXSMM_DNN_CONV_FUSE_BATCH_STATS;
#elif defined(USE_FUSED_MAX_STATS)
    conv_desc.fuse_ops = LIBXSMM_DNN_CONV_FUSE_MAX_STATS;
#elif defined(USE_FUSED_RELU_BWD)
    conv_desc.fuse_ops = LIBXSMM_DNN_CONV_FUSE_RELU_BWD;
#else
    conv_desc.fuse_ops = LIBXSMM_DNN_CONV_FUSE_NONE;
#endif
    /*conv_desc.options = LIBXSMM_DNN_CONV_OPTION_UPD_NO_FILTER_REDUCE;*/
    conv_desc.datatype_in = LIBXSMM_DNN_DATATYPE_I16;
    conv_desc.datatype_out = LIBXSMM_DNN_DATATYPE_F32;

    libxsmm_handle = libxsmm_dnn_create_conv_layer( conv_desc, &status );
    CHKERR_LIBXSMM_DNN( status );

    /* setup LIBXSMM buffers and filter */
    libxsmm_layout = libxsmm_dnn_create_tensor_datalayout( libxsmm_handle, LIBXSMM_DNN_REGULAR_INPUT, &status ); CHKERR_LIBXSMM_DNN( status );
    libxsmm_input  = libxsmm_dnn_link_tensor( libxsmm_layout,  input_libxsmm, &status ); CHKERR_LIBXSMM_DNN( status );
    libxsmm_dnn_destroy_tensor_datalayout( libxsmm_layout );

    libxsmm_layout = libxsmm_dnn_create_tensor_datalayout( libxsmm_handle, LIBXSMM_DNN_GRADIENT_INPUT, &status ); CHKERR_LIBXSMM_DNN( status );
    libxsmm_dinput = libxsmm_dnn_link_tensor( libxsmm_layout, dinput_libxsmm, &status ); CHKERR_LIBXSMM_DNN( status );
    libxsmm_dnn_destroy_tensor_datalayout( libxsmm_layout );

    libxsmm_layout = libxsmm_dnn_create_tensor_datalayout( libxsmm_handle, LIBXSMM_DNN_REGULAR_OUTPUT, &status ); CHKERR_LIBXSMM_DNN( status );
    libxsmm_output  = libxsmm_dnn_link_tensor( libxsmm_layout,  output_libxsmm, &status ); CHKERR_LIBXSMM_DNN( status );
    libxsmm_dnn_destroy_tensor_datalayout( libxsmm_layout );

    libxsmm_layout = libxsmm_dnn_create_tensor_datalayout( libxsmm_handle, LIBXSMM_DNN_GRADIENT_OUTPUT, &status ); CHKERR_LIBXSMM_DNN( status );
    libxsmm_doutput = libxsmm_dnn_link_tensor( libxsmm_layout, doutput_libxsmm, &status ); CHKERR_LIBXSMM_DNN( status );
    libxsmm_dnn_destroy_tensor_datalayout( libxsmm_layout );

    libxsmm_layout = libxsmm_dnn_create_tensor_datalayout( libxsmm_handle, LIBXSMM_DNN_REGULAR_FILTER, &status ); CHKERR_LIBXSMM_DNN( status );
    libxsmm_filter  = libxsmm_dnn_link_tensor( libxsmm_layout,  filter_libxsmm, &status ); CHKERR_LIBXSMM_DNN( status );
    libxsmm_dnn_destroy_tensor_datalayout( libxsmm_layout );

    libxsmm_layout = libxsmm_dnn_create_tensor_datalayout( libxsmm_handle, LIBXSMM_DNN_GRADIENT_FILTER, &status ); CHKERR_LIBXSMM_DNN( status );
    libxsmm_dfilter = libxsmm_dnn_link_tensor( libxsmm_layout, dfilter_libxsmm, &status ); CHKERR_LIBXSMM_DNN( status );
    libxsmm_dnn_destroy_tensor_datalayout( libxsmm_layout );

    libxsmm_layout = libxsmm_dnn_create_tensor_datalayout( libxsmm_handle, LIBXSMM_DNN_REGULAR_BIAS, &status ); CHKERR_LIBXSMM_DNN( status );
    libxsmm_bias  = libxsmm_dnn_link_tensor( libxsmm_layout,  bias_libxsmm, &status ); CHKERR_LIBXSMM_DNN( status );
    libxsmm_dnn_destroy_tensor_datalayout( libxsmm_layout );

    libxsmm_layout = libxsmm_dnn_create_tensor_datalayout( libxsmm_handle, LIBXSMM_DNN_GRADIENT_BIAS, &status ); CHKERR_LIBXSMM_DNN( status );
    libxsmm_dbias = libxsmm_dnn_link_tensor( libxsmm_layout, dbias_libxsmm, &status ); CHKERR_LIBXSMM_DNN( status );
    libxsmm_dnn_destroy_tensor_datalayout( libxsmm_layout );

    libxsmm_layout = libxsmm_dnn_create_tensor_datalayout( libxsmm_handle, LIBXSMM_DNN_REGULAR_FILTER_TRANS, &status ); CHKERR_LIBXSMM_DNN( status );
    libxsmm_filter_tr  = libxsmm_dnn_link_tensor( libxsmm_layout, filtertr_libxsmm, &status ); CHKERR_LIBXSMM_DNN( status );
    libxsmm_dnn_destroy_tensor_datalayout( libxsmm_layout );

#ifdef USE_FUSED_BATCH_STATS
    libxsmm_layout = libxsmm_dnn_create_tensor_datalayout( libxsmm_handle, LIBXSMM_DNN_BATCH_STATS, &status ); CHKERR_LIBXSMM_DNN( status );
    libxsmm_batchstats  = libxsmm_dnn_link_tensor( libxsmm_layout, batchstats_libxsmm, &status ); CHKERR_LIBXSMM_DNN( status );
    libxsmm_dnn_destroy_tensor_datalayout( libxsmm_layout );
#endif

#ifdef USE_FUSED_MAX_STATS
    libxsmm_layout = libxsmm_dnn_create_tensor_datalayout( libxsmm_handle, LIBXSMM_DNN_MAX_STATS_FWD, &status ); CHKERR_LIBXSMM_DNN( status );
    libxsmm_maxstats_fwd  = libxsmm_dnn_link_tensor( libxsmm_layout, maxstats_libxsmm_fwd, &status ); CHKERR_LIBXSMM_DNN( status );
    libxsmm_dnn_destroy_tensor_datalayout( libxsmm_layout );

    libxsmm_layout = libxsmm_dnn_create_tensor_datalayout( libxsmm_handle, LIBXSMM_DNN_MAX_STATS_BWD, &status ); CHKERR_LIBXSMM_DNN( status );
    libxsmm_maxstats_bwd  = libxsmm_dnn_link_tensor( libxsmm_layout, maxstats_libxsmm_bwd, &status ); CHKERR_LIBXSMM_DNN( status );
    libxsmm_dnn_destroy_tensor_datalayout( libxsmm_layout );

    libxsmm_layout = libxsmm_dnn_create_tensor_datalayout( libxsmm_handle, LIBXSMM_DNN_MAX_STATS_UPD, &status ); CHKERR_LIBXSMM_DNN( status );
    libxsmm_maxstats_upd  = libxsmm_dnn_link_tensor( libxsmm_layout, maxstats_libxsmm_upd, &status ); CHKERR_LIBXSMM_DNN( status );
    libxsmm_dnn_destroy_tensor_datalayout( libxsmm_layout );
#endif

    /* quantize input, filter, and Bias */
    libxsmm_dnn_quantize( naive_input_save, i16_naive_input,  nImg*nIfm*ifhp*ifwp, 2, &scf_input,  LIBXSMM_DNN_QUANT_FPHW_ROUND );
    libxsmm_dnn_quantize( naive_filter,     i16_naive_filter, nIfm*nOfm*kw*kh    , 2, &scf_filter, LIBXSMM_DNN_QUANT_FPHW_ROUND );

#ifdef USE_FORMATED_QUANT
    /* quantize and copy into LIBXSMM format */
    libxsmm_dnn_quantize_act( naive_input_save, input_libxsmm,  nImg, nIfm, ifhp, ifwp, 1, 8, 2, 2, &scf_input,  LIBXSMM_DNN_QUANT_FPHW_ROUND );
    libxsmm_dnn_quantize_fil( naive_filter,     filter_libxsmm, nOfm, nIfm, kw, kh, 1, 8, 1, 16, 2, 2, &scf_filter, LIBXSMM_DNN_QUANT_FPHW_ROUND );
#endif

    /* set scaling factors into tensors */
    libxsmm_dnn_set_qtensor_scf( libxsmm_input,  scf_input );
    libxsmm_dnn_set_qtensor_scf( libxsmm_filter, scf_filter );

    /* dequantize to check quantization error */
    libxsmm_dnn_dequantize( i16_naive_input,  dq_naive_input,  nImg*nIfm*ifhp*ifwp, scf_input );
    libxsmm_dnn_dequantize( i16_naive_filter, dq_naive_filter, nIfm*nOfm*kw*kh,     scf_filter );

    /* copy in data to LIBXSMM format */
    /* we can also use the layout functions and set the data on our
       own external to the library, @TODO, we plan to add an example here */
#ifndef USE_FORMATED_QUANT
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_copyin_tensor( libxsmm_input,  (void*)i16_naive_input,   LIBXSMM_DNN_TENSOR_FORMAT_NCHW ) );
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_copyin_tensor( libxsmm_filter, (void*)i16_naive_filter,  LIBXSMM_DNN_TENSOR_FORMAT_KCRS ) );
#endif
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_copyin_tensor( libxsmm_output, (void*)naive_output_save, LIBXSMM_DNN_TENSOR_FORMAT_NCHW ) );
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_copyin_tensor( libxsmm_bias,   (void*)naive_bias,        LIBXSMM_DNN_TENSOR_FORMAT_NCHW ) );
    zero_buf_i16(filtertr_libxsmm, nOfm*nIfm*kh*kw);
#ifdef FP32_BN_STATS
    zero_buf(batchstats_libxsmm, 2*nImg*nOfm);
#endif
#ifdef FP64_BN_STATS
    zero_buf((float *) batchstats_libxsmm, 4*nImg*nOfm);
#endif

    /* bind buffers and filter to handle */
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_bind_tensor( libxsmm_handle, libxsmm_input,      LIBXSMM_DNN_REGULAR_INPUT ) );
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_bind_tensor( libxsmm_handle, libxsmm_dinput,     LIBXSMM_DNN_GRADIENT_INPUT ) );
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_bind_tensor( libxsmm_handle, libxsmm_output,     LIBXSMM_DNN_REGULAR_OUTPUT ) );
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_bind_tensor( libxsmm_handle, libxsmm_doutput,    LIBXSMM_DNN_GRADIENT_OUTPUT ) );
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_bind_tensor( libxsmm_handle, libxsmm_filter,     LIBXSMM_DNN_REGULAR_FILTER ) );
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_bind_tensor( libxsmm_handle, libxsmm_dfilter,    LIBXSMM_DNN_GRADIENT_FILTER ) );
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_bind_tensor( libxsmm_handle, libxsmm_bias,       LIBXSMM_DNN_REGULAR_BIAS ) );
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_bind_tensor( libxsmm_handle, libxsmm_dbias,      LIBXSMM_DNN_GRADIENT_BIAS ) );
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_bind_tensor( libxsmm_handle, libxsmm_filter_tr,  LIBXSMM_DNN_REGULAR_FILTER_TRANS ) );
#ifdef USE_FUSED_BATCH_STATS
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_bind_tensor( libxsmm_handle, libxsmm_batchstats, LIBXSMM_DNN_BATCH_STATS ) );
#endif
#ifdef USE_FUSED_MAX_STATS
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_bind_tensor( libxsmm_handle, libxsmm_maxstats_fwd, LIBXSMM_DNN_MAX_STATS_FWD ) );
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_bind_tensor( libxsmm_handle, libxsmm_maxstats_bwd, LIBXSMM_DNN_MAX_STATS_BWD ) );
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_bind_tensor( libxsmm_handle, libxsmm_maxstats_upd, LIBXSMM_DNN_MAX_STATS_UPD ) );
#endif

    /* let's allocate and bind scratch */
    scratch_size = libxsmm_dnn_get_scratch_size( libxsmm_handle, LIBXSMM_DNN_COMPUTE_KIND_ALL, &status );
    CHKERR_LIBXSMM_DNN( status );
    scratch = libxsmm_aligned_malloc( scratch_size, 2097152 );
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_bind_scratch( libxsmm_handle, LIBXSMM_DNN_COMPUTE_KIND_ALL, scratch ) );
    /* set scratch to bogus to make sure that libxsmm takes care of zeroing internally */
    init_buf( (float*)scratch, scratch_size/4, 0, 0 );

    if ((type == 'A' || type == 'F') && LIBXSMM_NEQ(0, check)) {
      printf("##########################################\n");
      printf("#   Correctness - FWD (custom-Storage)   #\n");
      printf("##########################################\n");
      /* run LIBXSMM convolutions */
#if defined(_OPENMP)
#     pragma omp parallel
#endif
      {
#if defined(_OPENMP)
        const int tid = omp_get_thread_num();
#else
        const int tid = 0;
#endif
        CHKERR_LIBXSMM_DNN( libxsmm_dnn_execute_st( libxsmm_handle, LIBXSMM_DNN_COMPUTE_KIND_FWD, 0, tid ) );
      }
      /* copy out data */
      CHKERR_LIBXSMM_DNN( libxsmm_dnn_copyout_tensor( libxsmm_output, (void*)naive_libxsmm_output, LIBXSMM_DNN_TENSOR_FORMAT_NCHW ) );

      /* norms quantization */
      libxsmm_matdiff(LIBXSMM_DATATYPE_F32, nImg*nIfm*ifhp*ifwp, 1, naive_input_save, dq_naive_input, 0, 0, &norms_quant);
      printf("Input Quantization:\n");
      printf("L1 reference  : %.25g\n", norms_quant.l1_ref);
      printf("L1 test       : %.25g\n", norms_quant.l1_tst);
      printf("L2 abs.error  : %.24f\n", norms_quant.l2_abs);
      printf("L2 rel.error  : %.24f\n", norms_quant.l2_rel);
      printf("Linf abs.error: %.24f\n", norms_quant.linf_abs);
      printf("Linf rel.error: %.24f\n", norms_quant.linf_rel);
      printf("Check-norm    : %.24f\n", norms_quant.normf_rel);

      libxsmm_matdiff(LIBXSMM_DATATYPE_F32, nIfm*nOfm*kw*kh, 1, naive_filter, dq_naive_filter, 0, 0, &norms_quant);
      printf("Filter Quantization:\n");
      printf("L1 reference  : %.25g\n", norms_quant.l1_ref);
      printf("L1 test       : %.25g\n", norms_quant.l1_tst);
      printf("L2 abs.error  : %.24f\n", norms_quant.l2_abs);
      printf("L2 rel.error  : %.24f\n", norms_quant.l2_rel);
      printf("Linf abs.error: %.24f\n", norms_quant.linf_abs);
      printf("Linf rel.error: %.24f\n", norms_quant.linf_rel);
      printf("Check-norm    : %.24f\n", norms_quant.normf_rel);

      /* compare */
      libxsmm_matdiff(LIBXSMM_DATATYPE_F32, nImg*nOfm*ofhp*ofwp, 1, naive_output, naive_libxsmm_output, 0, 0, &norms_fwd);
      printf("Output:\n");
      printf("L1 reference  : %.25g\n", norms_fwd.l1_ref);
      printf("L1 test       : %.25g\n", norms_fwd.l1_tst);
      printf("L2 abs.error  : %.24f\n", norms_fwd.l2_abs);
      printf("L2 rel.error  : %.24f\n", norms_fwd.l2_rel);
      printf("Linf abs.error: %.24f\n", norms_fwd.linf_abs);
      printf("Linf rel.error: %.24f\n", norms_fwd.linf_rel);
      printf("Check-norm    : %.24f\n", norms_fwd.normf_rel);
      libxsmm_matdiff_reduce(&diff, &norms_fwd);

#ifdef USE_FUSED_MAX_STATS
      {
        int img_i = 0;
        int ch_i = 0;
        int pxl_i = 0;
        float max_naive = 0.0;
        float max_libxsmm = 0.0;
        LIBXSMM_VLA_DECL(3, float, val_naive, naive_output, nOfm, ofhp*ofwp);
        for ( img_i = 0; img_i < nImg; ++img_i ) {
          for ( ch_i = 0; ch_i < nOfm; ++ch_i ) {
            for ( pxl_i = 0; pxl_i < ofhp*ofwp; ++pxl_i ) {
              max_naive = LIBXSMM_MAX( max_naive , fabs(val_naive[img_i][ch_i][pxl_i]) );
            }
          }
        }
        for ( img_i = 0; img_i < nImg; ++img_i ) {
          for ( ch_i = 0; ch_i < 16; ++ch_i ) {
            max_libxsmm = LIBXSMM_MAX( max_libxsmm, maxstats_libxsmm_fwd[img_i*16+ch_i]);
          }
        }
        printf("absolute max value:\n");
        printf("Referen. max abs FWD value: %.25f\n", max_naive);
        printf("LIBXSMM  max abs FWD value: %.25f\n", max_libxsmm);
        printf("Linf abs.error  : %.24f\n\n", max_naive-max_libxsmm);
      }
#endif
#if defined(USE_FUSED_BATCH_STATS)
      {
        float *ch_sum, *ch_sum_fuse;
        float *ch_sum2, *ch_sum2_fuse;
        int img_i = 0;
        int ch_i = 0;
        int ch_j = 0;
        int pxl_i = 0;
#ifdef FP32_BN_STATS
        LIBXSMM_VLA_DECL(4, float, sum_fuse,  batchstats_libxsmm, nOfm/16, nImg, 16);
#endif
#ifdef FP64_BN_STATS
        LIBXSMM_VLA_DECL(4, double, sum_fuse,  batchstats_libxsmm, nOfm/16, nImg, 16);
#endif
        LIBXSMM_VLA_DECL(3, float, sum_naive, naive_output,       nOfm, ofhp*ofwp);

        ch_sum       = (float*) malloc(nOfm*sizeof(float));
        ch_sum_fuse  = (float*) malloc(nOfm*sizeof(float));
        ch_sum2      = (float*) malloc(nOfm*sizeof(float));
        ch_sum2_fuse = (float*) malloc(nOfm*sizeof(float));

        for ( ch_i = 0; ch_i < nOfm; ++ch_i ) {
          ch_sum_fuse[ch_i] = 0.0f;
          ch_sum2_fuse[ch_i] = 0.0f;
          ch_sum[ch_i] = 0.0f;
          ch_sum2[ch_i] = 0.0f;
        }
        for ( ch_i = 0; ch_i < nOfm/16; ++ch_i ) {
          for ( img_i = 0; img_i < nImg; ++img_i ) {
            for ( ch_j = 0; ch_j < 16; ++ch_j ) {
#ifdef FP32_BN_STATS
              ch_sum_fuse[(ch_i*16) + ch_j]  += sum_fuse[0][ch_i][img_i][ch_j];
              ch_sum2_fuse[(ch_i*16) + ch_j] += sum_fuse[1][ch_i][img_i][ch_j];
#endif
#ifdef FP64_BN_STATS
              ch_sum_fuse[(ch_i*16) + ch_j]  += (float) sum_fuse[0][ch_i][img_i][ch_j];
              ch_sum2_fuse[(ch_i*16) + ch_j] += (float) sum_fuse[1][ch_i][img_i][ch_j];
#endif
            }
          }
        }
        for ( img_i = 0; img_i < nImg; ++img_i ) {
          for ( ch_i = 0; ch_i < nOfm; ++ch_i ) {
            for ( pxl_i = 0; pxl_i < ofhp*ofwp; ++pxl_i ) {
              ch_sum[ch_i]  += sum_naive[img_i][ch_i][pxl_i];
              ch_sum2[ch_i] += (sum_naive[img_i][ch_i][pxl_i]*sum_naive[img_i][ch_i][pxl_i]);
            }
          }
        }

        libxsmm_matdiff(LIBXSMM_DATATYPE_F32, nOfm, 1, ch_sum, ch_sum_fuse, 0, 0, &norms_batchstats);
        printf("Channel Sum:\n");
        printf("L1 reference  : %.25g\n", norms_batchstats.l1_ref);
        printf("L1 test       : %.25g\n", norms_batchstats.l1_tst);
        printf("L2 abs.error  : %.24f\n", norms_batchstats.l2_abs);
        printf("L2 rel.error  : %.24f\n", norms_batchstats.l2_rel);
        printf("Linf abs.error: %.24f\n", norms_batchstats.linf_abs);
        printf("Linf rel.error: %.24f\n", norms_batchstats.linf_rel);
        printf("Check-norm    : %.24f\n", norms_batchstats.normf_rel);

        libxsmm_matdiff(LIBXSMM_DATATYPE_F32, nOfm, 1, ch_sum2, ch_sum2_fuse, 0, 0, &norms_batchstats);
        printf("Channel Sum2:\n");
        printf("L1 reference  : %.25g\n", norms_batchstats.l1_ref);
        printf("L1 test       : %.25g\n", norms_batchstats.l1_tst);
        printf("L2 abs.error  : %.24f\n", norms_batchstats.l2_abs);
        printf("L2 rel.error  : %.24f\n", norms_batchstats.l2_rel);
        printf("Linf abs.error: %.24f\n", norms_batchstats.linf_abs);
        printf("Linf rel.error: %.24f\n", norms_batchstats.linf_rel);
        printf("Check-norm    : %.24f\n", norms_batchstats.normf_rel);

        free(ch_sum);
        free(ch_sum2);
        free(ch_sum_fuse);
        free(ch_sum2_fuse);
      }
#endif
    }

    if ( (type == 'A' || type == 'B') && (nIfm > 3) && LIBXSMM_NEQ(0, check) ) {
      printf("##########################################\n");
      printf("#   Correctness - BWD (custom-Storage)   #\n");
      printf("##########################################\n");

      /* quantize input, filter, and Bias */
      libxsmm_dnn_quantize( naive_output_bp, i16_naive_doutput, nImg*nOfm*ofhp*ofwp, 2, &scf_doutput,  LIBXSMM_DNN_QUANT_FPHW_ROUND );

      /* set scaling factors into tensors */
      libxsmm_dnn_set_qtensor_scf( libxsmm_doutput,  scf_doutput );

      /* dequantize to check quantization error */
      libxsmm_dnn_dequantize( i16_naive_doutput,  dq_naive_doutput, nImg*nOfm*ofhp*ofwp, scf_doutput );

      /* let's do some additional init such that we can run passes standalone */
      CHKERR_LIBXSMM_DNN( libxsmm_dnn_copyin_tensor(    libxsmm_doutput, (void*)i16_naive_doutput, LIBXSMM_DNN_TENSOR_FORMAT_NCHW ) );
      CHKERR_LIBXSMM_DNN( libxsmm_dnn_copyin_tensor(    libxsmm_dinput,  (void*)naive_input_save,  LIBXSMM_DNN_TENSOR_FORMAT_NCHW ) );
#if defined(USE_BWD_NO_FILTER_TRANSPOSE_OVERWRITE)
      CHKERR_LIBXSMM_DNN( libxsmm_dnn_trans_reg_filter( libxsmm_handle ) );
#endif

      /* run LIBXSMM convolutions */
#if defined(_OPENMP)
#     pragma omp parallel
#endif
      {
#if defined(_OPENMP)
        const int tid = omp_get_thread_num();
#else
        const int tid = 0;
#endif
        CHKERR_LIBXSMM_DNN( libxsmm_dnn_execute_st( libxsmm_handle, LIBXSMM_DNN_COMPUTE_KIND_BWD, 0, tid ) );
      }

      /* copy out data */
      CHKERR_LIBXSMM_DNN( libxsmm_dnn_copyout_tensor( libxsmm_dinput, (void*)naive_libxsmm_input, LIBXSMM_DNN_TENSOR_FORMAT_NCHW ) );

      /* norms quantization */
      libxsmm_matdiff(LIBXSMM_DATATYPE_F32, nImg*nOfm*ofhp*ofwp, 1, naive_output_bp, dq_naive_doutput, 0, 0, &norms_quant);
      printf("del-Output Quantization:\n");
      printf("L1 reference  : %.25g\n", norms_quant.l1_ref);
      printf("L1 test       : %.25g\n", norms_quant.l1_tst);
      printf("L2 abs.error  : %.24f\n", norms_quant.l2_abs);
      printf("L2 rel.error  : %.24f\n", norms_quant.l2_rel);
      printf("Linf abs.error: %.24f\n", norms_quant.linf_abs);
      printf("Linf rel.error: %.24f\n", norms_quant.linf_rel);
      printf("Check-norm    : %.24f\n", norms_quant.normf_rel);

      libxsmm_matdiff(LIBXSMM_DATATYPE_F32, nIfm*nOfm*kw*kh, 1, naive_filter, dq_naive_filter, 0, 0, &norms_quant);
      printf("Filter Quantization:\n");
      printf("L1 reference  : %.25g\n", norms_quant.l1_ref);
      printf("L1 test       : %.25g\n", norms_quant.l1_tst);
      printf("L2 abs.error  : %.24f\n", norms_quant.l2_abs);
      printf("L2 rel.error  : %.24f\n", norms_quant.l2_rel);
      printf("Linf abs.error: %.24f\n", norms_quant.linf_abs);
      printf("Linf rel.error: %.24f\n", norms_quant.linf_rel);
      printf("Check-norm    : %.24f\n", norms_quant.normf_rel);

      /* compare */
      libxsmm_matdiff(LIBXSMM_DATATYPE_F32, nImg*nIfm*ifhp*ifwp, 1, naive_input, naive_libxsmm_input, 0, 0, &norms_bwd);
      printf("del-Input:\n");
      printf("L1 reference  : %.25g\n", norms_bwd.l1_ref);
      printf("L1 test       : %.25g\n", norms_bwd.l1_tst);
      printf("L2 abs.error  : %.24f\n", norms_bwd.l2_abs);
      printf("L2 rel.error  : %.24f\n", norms_bwd.l2_rel);
      printf("Linf abs.error: %.24f\n", norms_bwd.linf_abs);
      printf("Linf rel.error: %.24f\n", norms_bwd.linf_rel);
      printf("Check-norm    : %.24f\n", norms_bwd.normf_rel);
      libxsmm_matdiff_reduce(&diff, &norms_bwd);

#ifdef USE_FUSED_MAX_STATS
      {
        int img_i = 0;
        int ch_i = 0;
        int pxl_i = 0;
        float max_naive = 0.0;
        float max_libxsmm = 0.0;
        LIBXSMM_VLA_DECL(3, float, val_naive, naive_input, nIfm, ifhp*ifwp);
        for ( img_i = 0; img_i < nImg; ++img_i ) {
          for ( ch_i = 0; ch_i < nIfm; ++ch_i ) {
            for ( pxl_i = 0; pxl_i < ifhp*ifwp; ++pxl_i ) {
              max_naive = LIBXSMM_MAX( max_naive , fabs(val_naive[img_i][ch_i][pxl_i]) );
            }
          }
        }
        for ( img_i = 0; img_i < nImg; ++img_i ) {
          for ( ch_i = 0; ch_i < 16; ++ch_i ) {
            max_libxsmm = LIBXSMM_MAX( max_libxsmm, maxstats_libxsmm_bwd[img_i*16+ch_i]);
          }
        }
        printf("absolute max value:\n");
        printf("Referen. max abs BWD value: %.25f\n", max_naive);
        printf("LIBXSMM  max abs BWD value: %.25f\n", max_libxsmm);
        printf("Linf abs.error  : %.24f\n\n", max_naive-max_libxsmm);
      }
#endif
    }

    if ((type == 'A' || type == 'U') && LIBXSMM_NEQ(0, check)) {
      printf("##########################################\n");
      printf("#   Correctness - UPD (custom-Storage)   #\n");
      printf("##########################################\n");
      /* quantize input, filter, and Bias */
      libxsmm_dnn_quantize( naive_input_save, i16_naive_input,   nImg*nIfm*ifhp*ifwp, 2, &scf_input,    LIBXSMM_DNN_QUANT_FPHW_ROUND );
      libxsmm_dnn_quantize( naive_output_wu,  i16_naive_doutput, nImg*nOfm*ofhp*ofwp, 2, &scf_doutput,  LIBXSMM_DNN_QUANT_FPHW_ROUND );

      /* set scaling factors into tensors */
      libxsmm_dnn_set_qtensor_scf( libxsmm_input,  scf_input );
      libxsmm_dnn_set_qtensor_scf( libxsmm_doutput,  scf_doutput );

      /* dequantize to check quantization error */
      libxsmm_dnn_dequantize( i16_naive_input,    dq_naive_input,   nImg*nIfm*ifhp*ifwp, scf_input );
      libxsmm_dnn_dequantize( i16_naive_doutput,  dq_naive_doutput, nImg*nOfm*ofhp*ofwp, scf_doutput );

      /* let's do some additional init such that we can run passes standalone */
      CHKERR_LIBXSMM_DNN( libxsmm_dnn_copyin_tensor( libxsmm_input,   (void*)i16_naive_input,   LIBXSMM_DNN_TENSOR_FORMAT_NCHW ) );
      CHKERR_LIBXSMM_DNN( libxsmm_dnn_copyin_tensor( libxsmm_doutput, (void*)i16_naive_doutput, LIBXSMM_DNN_TENSOR_FORMAT_NCHW ) );
      CHKERR_LIBXSMM_DNN( libxsmm_dnn_copyin_tensor( libxsmm_dfilter, (void*)naive_filter,      LIBXSMM_DNN_TENSOR_FORMAT_KCRS ) );

      /* run LIBXSMM convolutions */
#if defined(_OPENMP)
#     pragma omp parallel
#endif
      {
#if defined(_OPENMP)
        const int tid = omp_get_thread_num();
#else
        const int tid = 0;
#endif
        CHKERR_LIBXSMM_DNN( libxsmm_dnn_execute_st( libxsmm_handle, LIBXSMM_DNN_COMPUTE_KIND_UPD, 0, tid ) );
      }
      if (conv_desc.options == LIBXSMM_DNN_CONV_OPTION_UPD_NO_FILTER_REDUCE) {
        CHKERR_LIBXSMM_DNN( libxsmm_dnn_reduce_wu_filters( libxsmm_handle, LIBXSMM_DNN_GRADIENT_FILTER ) );
      }

      /* copy out data */
      CHKERR_LIBXSMM_DNN( libxsmm_dnn_copyout_tensor( libxsmm_dfilter, (void*)naive_libxsmm_filter, LIBXSMM_DNN_TENSOR_FORMAT_KCRS ) );

      libxsmm_matdiff(LIBXSMM_DATATYPE_F32, nImg*nIfm*ifhp*ifwp, 1, naive_input_save, dq_naive_input, 0, 0, &norms_quant);
      printf("Input Quantization:\n");
      printf("L1 reference  : %.25g\n", norms_quant.l1_ref);
      printf("L1 test       : %.25g\n", norms_quant.l1_tst);
      printf("L2 abs.error  : %.24f\n", norms_quant.l2_abs);
      printf("L2 rel.error  : %.24f\n", norms_quant.l2_rel);
      printf("Linf abs.error: %.24f\n", norms_quant.linf_abs);
      printf("Linf rel.error: %.24f\n", norms_quant.linf_rel);
      printf("Check-norm    : %.24f\n", norms_quant.normf_rel);

      /* norms quantization */
      libxsmm_matdiff(LIBXSMM_DATATYPE_F32, nImg*nOfm*ofhp*ofwp, 1, naive_output_bp, dq_naive_doutput, 0, 0, &norms_quant);
      printf("del-Output Quantization:\n");
      printf("L1 reference  : %.25g\n", norms_quant.l1_ref);
      printf("L1 test       : %.25g\n", norms_quant.l1_tst);
      printf("L2 abs.error  : %.24f\n", norms_quant.l2_abs);
      printf("L2 rel.error  : %.24f\n", norms_quant.l2_rel);
      printf("Linf abs.error: %.24f\n", norms_quant.linf_abs);
      printf("Linf rel.error: %.24f\n", norms_quant.linf_rel);
      printf("Check-norm    : %.24f\n", norms_quant.normf_rel);

      /* compare */
      libxsmm_matdiff(LIBXSMM_DATATYPE_F32, nOfm*nIfm*kh*kw, 1, naive_filter_wu, naive_libxsmm_filter, 0, 0, &norms_upd);
      printf("del-Filter:\n");
      printf("L1 reference  : %.25g\n", norms_upd.l1_ref);
      printf("L1 test       : %.25g\n", norms_upd.l1_tst);
      printf("L2 abs.error  : %.24f\n", norms_upd.l2_abs);
      printf("L2 rel.error  : %.24f\n", norms_upd.l2_rel);
      printf("Linf abs.error: %.24f\n", norms_upd.linf_abs);
      printf("Linf rel.error: %.24f\n", norms_upd.linf_rel);
      printf("Check-norm    : %.24f\n", norms_upd.normf_rel);
      libxsmm_matdiff_reduce(&diff, &norms_upd);

#ifdef USE_FUSED_MAX_STATS
      {
        int thread_i = 0;
        int entry_i = 0;
        int c,k,r,s;
        float max_naive = 0.0;
        float max_libxsmm = 0.0;

        for ( entry_i = 0; entry_i < nOfm*nIfm*kh*kw; ++entry_i) {
          max_naive = LIBXSMM_MAX( max_naive , fabs(naive_filter_wu[entry_i]));
        }

        for ( thread_i = 0; thread_i < nImg; ++thread_i) {
          for ( entry_i = 0; entry_i < 16; ++entry_i ) {
            max_libxsmm = LIBXSMM_MAX( max_libxsmm, maxstats_libxsmm_upd[thread_i*16+entry_i]);
          }
        }

        printf("absolute max value:\n");
        printf("Referen. max abs UPD value: %.25f\n", max_naive);
        printf("LIBXSMM  max abs UPD value: %.25f\n", max_libxsmm);
        printf("Linf abs.error  : %.24f\n\n", max_naive-max_libxsmm);
      }
#endif
    }

    if (type == 'A' || type == 'F') {
      printf("##########################################\n");
      printf("#   Performance - FWD (custom-Storage)   #\n");
      printf("##########################################\n");
      /* run LIBXSMM convolution for performance */
      l_start = libxsmm_timer_tick();
#if defined(_OPENMP)
#   pragma omp parallel private(i)
#endif
      {
#if defined(_OPENMP)
        const int tid = omp_get_thread_num();
#else
        const int tid = 0;
#endif
        for (i = 0; i < iters; ++i) {
          libxsmm_dnn_execute_st( libxsmm_handle, LIBXSMM_DNN_COMPUTE_KIND_FWD, 0, tid );
        }
      }
      l_end = libxsmm_timer_tick();
      l_total = libxsmm_timer_duration(l_start, l_end);
      flops = (double)nImg * (double)nIfm * (double)nOfm * (double)ofh * (double)ofw * (double)(2 * kh * kw) * (double)iters;

      printf("GFLOP  = %.5g\n", flops*1e-9/(double)iters);
      printf("fp time = %.5g\n", ((double)(l_total/iters)));
      printf("GFLOPS  = %.5g\n", (flops*1e-9)/l_total);

      printf("PERFDUMP,FP,%s,%i,%i,%i,%i,%i,%i,%i,%i,%i,%i,%i,%.5g,%.5g,%f,%f,%f,%f,%f,%f,%f\n", LIBXSMM_VERSION, nThreads, nImg, nIfm, nOfm,
        ifw, ifh, kw, kh, stride, padw, padh, ((double)(l_total/iters)), (flops*1e-9)/l_total, norms_fwd.l1_ref, norms_fwd.l1_tst,
        norms_fwd.l2_abs, norms_fwd.l2_rel, norms_fwd.linf_abs, norms_fwd.linf_rel, norms_fwd.normf_rel);
    }

    if ( (type == 'A' || type == 'B') && (nIfm > 3) ) {
      printf("##########################################\n");
      printf("#   Performance - BWD (custom-Storage)   #\n");
      printf("##########################################\n");
      /* run LIBXSMM convolution for performance */
      l_start = libxsmm_timer_tick();

#if defined(_OPENMP)
#   pragma omp parallel  private(i)
#endif
      {
#if defined(_OPENMP)
        const int tid = omp_get_thread_num();
#else
        const int tid = 0;
#endif
        for (i = 0; i < iters; ++i) {
          libxsmm_dnn_execute_st( libxsmm_handle, LIBXSMM_DNN_COMPUTE_KIND_BWD, 0, tid );
        }
      }
      l_end = libxsmm_timer_tick();
      l_total = libxsmm_timer_duration(l_start, l_end);
      flops = (double)nImg * (double)nIfm * (double)nOfm * (double)ofh * (double)ofw * (double)(2 * kh * kw) * (double)iters;

      printf("GFLOP  = %.5g\n", flops*1e-9/(double)iters);
      printf("bp time = %.5g\n", ((double)(l_total/iters)));
      printf("GFLOPS  = %.5g\n", (flops*1e-9)/l_total);

      printf("PERFDUMP,BP,%s,%i,%i,%i,%i,%i,%i,%i,%i,%i,%i,%i,%.5g,%.5g,%f,%f,%f,%f,%f,%f,%f\n", LIBXSMM_VERSION, nThreads, nImg, nIfm, nOfm,
        ifw, ifh, kw, kh, stride, padw, padh, ((double)(l_total/iters)), (flops*1e-9)/l_total, norms_bwd.l1_ref, norms_bwd.l1_tst,
        norms_bwd.l2_abs, norms_bwd.l2_rel, norms_bwd.linf_abs, norms_bwd.linf_rel, norms_bwd.normf_rel);
    }

    if (type == 'A' || type == 'U') {
      printf("##########################################\n");
      printf("#   Performance - UPD (custom-Storage)   #\n");
      printf("##########################################\n");
      /* run LIBXSMM convolution for performance */
      l_start = libxsmm_timer_tick();

#if defined(_OPENMP)
#   pragma omp parallel private(i)
#endif
      {
#if defined(_OPENMP)
        const int tid = omp_get_thread_num();
#else
        const int tid = 0;
#endif
        for (i = 0; i < iters; ++i) {
          libxsmm_dnn_execute_st( libxsmm_handle, LIBXSMM_DNN_COMPUTE_KIND_UPD, 0, tid );
          if (conv_desc.options == LIBXSMM_DNN_CONV_OPTION_UPD_NO_FILTER_REDUCE) {
            CHKERR_LIBXSMM_DNN( libxsmm_dnn_reduce_wu_filters( libxsmm_handle, LIBXSMM_DNN_GRADIENT_FILTER ) );
          }
        }
      }
      l_end = libxsmm_timer_tick();
      l_total = libxsmm_timer_duration(l_start, l_end);
      flops = (double)nImg * (double)nIfm * (double)nOfm * (double)ofh * (double)ofw * (double)(2 * kh * kw) * (double)iters;

      printf("GFLOP  = %.5g\n", flops*1e-9/(double)iters);
      printf("wu time = %.5g\n", ((double)(l_total/iters)));
      printf("GFLOPS  = %.5g\n", (flops*1e-9)/l_total);

      printf("PERFDUMP,WU,%s,%i,%i,%i,%i,%i,%i,%i,%i,%i,%i,%i,%.5g,%.5g,%f,%f,%f,%f,%f,%f,%f\n", LIBXSMM_VERSION, nThreads, nImg, nIfm, nOfm,
        ifw, ifh, kw, kh, stride, padw, padh, ((double)(l_total/iters)), (flops*1e-9)/l_total, norms_upd.l1_ref, norms_upd.l1_tst,
        norms_upd.l2_abs, norms_upd.l2_rel, norms_upd.linf_abs, norms_upd.linf_rel, norms_upd.normf_rel);
    }

    /* clean-up */
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_release_scratch( libxsmm_handle, LIBXSMM_DNN_COMPUTE_KIND_ALL ) );
    libxsmm_free(scratch);
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_release_tensor( libxsmm_handle, LIBXSMM_DNN_REGULAR_INPUT ) );
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_release_tensor( libxsmm_handle, LIBXSMM_DNN_GRADIENT_INPUT ) );
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_release_tensor( libxsmm_handle, LIBXSMM_DNN_REGULAR_OUTPUT ) );
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_release_tensor( libxsmm_handle, LIBXSMM_DNN_GRADIENT_OUTPUT ) );
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_release_tensor( libxsmm_handle, LIBXSMM_DNN_REGULAR_FILTER ) );
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_release_tensor( libxsmm_handle, LIBXSMM_DNN_GRADIENT_FILTER ) );
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_release_tensor( libxsmm_handle, LIBXSMM_DNN_REGULAR_BIAS ) );
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_release_tensor( libxsmm_handle, LIBXSMM_DNN_GRADIENT_BIAS ) );
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_release_tensor( libxsmm_handle, LIBXSMM_DNN_REGULAR_FILTER_TRANS ) );
#ifdef USE_FUSED_BATCH_STATS
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_release_tensor( libxsmm_handle, LIBXSMM_DNN_BATCH_STATS ) );
#endif
#ifdef USE_FUSED_MAX_STATS
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_release_tensor( libxsmm_handle, LIBXSMM_DNN_MAX_STATS_FWD ) );
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_release_tensor( libxsmm_handle, LIBXSMM_DNN_MAX_STATS_BWD ) );
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_release_tensor( libxsmm_handle, LIBXSMM_DNN_MAX_STATS_UPD ) );
#endif
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_destroy_tensor( libxsmm_input ) );
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_destroy_tensor( libxsmm_output ) );
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_destroy_tensor( libxsmm_filter ) );
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_destroy_tensor( libxsmm_dinput ) );
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_destroy_tensor( libxsmm_doutput ) );
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_destroy_tensor( libxsmm_dfilter ) );
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_destroy_tensor( libxsmm_bias ) );
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_destroy_tensor( libxsmm_dbias ) );
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_destroy_tensor( libxsmm_filter_tr ) );
#ifdef USE_FUSED_BATCH_STATS
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_destroy_tensor( libxsmm_batchstats ) );
#endif
#ifdef USE_FUSED_MAX_STATS
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_destroy_tensor( libxsmm_maxstats_fwd ) );
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_destroy_tensor( libxsmm_maxstats_bwd ) );
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_destroy_tensor( libxsmm_maxstats_upd ) );
#endif
    CHKERR_LIBXSMM_DNN( libxsmm_dnn_destroy_conv_layer( libxsmm_handle ) );
  }

  /* deallocate data */
  libxsmm_free(naive_input);
  libxsmm_free(naive_input_save);
  libxsmm_free(naive_output);
  libxsmm_free(naive_output_save);
  libxsmm_free(naive_output_bp);
  libxsmm_free(naive_output_wu);
  libxsmm_free(naive_libxsmm_output);
  libxsmm_free(naive_libxsmm_input);
  libxsmm_free(naive_filter);
  libxsmm_free(naive_filter_save);
  libxsmm_free(naive_filter_wu);
  libxsmm_free(naive_libxsmm_filter);
  libxsmm_free(input_libxsmm);
  libxsmm_free(filter_libxsmm);
  libxsmm_free(output_libxsmm);
  libxsmm_free(dinput_libxsmm);
  libxsmm_free(dfilter_libxsmm);
  libxsmm_free(doutput_libxsmm);
  libxsmm_free(filtertr_libxsmm);
  libxsmm_free(batchstats_libxsmm);
  libxsmm_free(naive_bias);
  libxsmm_free(naive_dbias);
  libxsmm_free(bias_libxsmm);
  libxsmm_free(dbias_libxsmm);
  libxsmm_free(i16_naive_input);
  libxsmm_free(i16_naive_filter);
  libxsmm_free(i16_naive_doutput);
  libxsmm_free(dq_naive_input);
  libxsmm_free(dq_naive_filter);
  libxsmm_free(dq_naive_doutput);

  { const char *const env_check_scale = getenv("CHECK_SCALE");
    const double check_scale = LIBXSMM_ABS(0 == env_check_scale ? 100.0 : atof(env_check_scale));
    if (LIBXSMM_NEQ(0, check) && (check < 100.0 * check_scale * diff.normf_rel) && (global_status == LIBXSMM_DNN_SUCCESS)) {
      fprintf(stderr, "FAILED with an error of %f%%!\n", 100.0 * diff.normf_rel);
      exit(EXIT_FAILURE);
    }
  }

  /* some empty lines at the end */
  printf("\n\n\n");

  return EXIT_SUCCESS;
}

