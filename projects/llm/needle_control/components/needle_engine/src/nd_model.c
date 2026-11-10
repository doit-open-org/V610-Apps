#include "nd_model.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "nd_quant.h"

uint64_t nd_prof[ND_P_COUNT];

#ifdef ND_PROFILE
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#define ND_NOW_US() ((uint64_t)esp_timer_get_time())
#else
#include <time.h>
#define ND_NOW_US() ((uint64_t)(clock() * (1000000.0 / CLOCKS_PER_SEC)))
#endif
#define ND_T0(v) uint64_t v = ND_NOW_US()
#define ND_T1(v, slot) nd_prof[slot] += ND_NOW_US() - (v)
#else
#define ND_T0(v) ((void)0)
#define ND_T1(v, slot) ((void)0)
#endif

#define ND_EPS      1e-6f
#define ND_EG_HIST  16      /* >= (taps-1)*dilation + 1 = 10, power of two */
#define ND_SINKHORN 20

/* Engram hash constants (architecture.py). */
#define ND_EG_SEED  0x9E3779B9u
#define ND_EG_PRIME 0x01000193u

/* ------------------------------------------------------------------ utils */

static float sigmoidf_(float x)
{
    if (x >= 0.0f) {
        float e = nd_expf(-x);
        return 1.0f / (1.0f + e);
    }
    {
        float e = nd_expf(x);
        return e / (1.0f + e);
    }
}

/* FP16 tensors are read element-wise; they are small (norm scales, gates). */
static float fp16_get(const nd_model *m, const nd_tensor *t, size_t i)
{
    const uint16_t *p = (const uint16_t *)nd_cact_data(&m->c, t);
    return nd_f16(p[i]);
}

/* x * rsqrt(mean(x^2) + eps) */
static void rms_unit(const float *x, uint32_t n, float *out)
{
    float    ss = 0.0f;
    uint32_t i;
    for (i = 0; i < n; i++)
        ss += x[i] * x[i];
    {
        float inv = 1.0f / sqrtf(ss / (float)n + ND_EPS);
        for (i = 0; i < n; i++)
            out[i] = x[i] * inv;
    }
}

/* ZCRMSNorm: (1 + scale) * x / sqrt(mean(x^2) + eps) */
static void zcrms(const nd_model *m, const nd_tensor *scale, const float *x,
                  uint32_t n, float *out)
{
    const uint16_t *s  = (const uint16_t *)nd_cact_data(&m->c, scale);
    float           ss = 0.0f;
    uint32_t        i;

    for (i = 0; i < n; i++)
        ss += x[i] * x[i];
    {
        float inv = 1.0f / sqrtf(ss / (float)n + ND_EPS);
        for (i = 0; i < n; i++)
            out[i] = (1.0f + nd_f16(s[i])) * x[i] * inv;
    }
}

/* In-place per-head ZCRMSNorm over `dim`, shared scale across heads. */
static void zcrms_heads(const nd_model *m, const nd_tensor *scale, float *x,
                        uint32_t nheads, uint32_t dim)
{
    const uint16_t *s = (const uint16_t *)nd_cact_data(&m->c, scale);
    uint32_t        h, i;

    for (h = 0; h < nheads; h++) {
        float *v  = x + (size_t)h * dim;
        float  ss = 0.0f;
        for (i = 0; i < dim; i++)
            ss += v[i] * v[i];
        {
            float inv = 1.0f / sqrtf(ss / (float)dim + ND_EPS);
            for (i = 0; i < dim; i++)
                v[i] = (1.0f + nd_f16(s[i])) * v[i] * inv;
        }
    }
}

/* GPT-NeoX style half-split rotary, against cos/sin precomputed once per
 * token (the position is the same for all layers). Applied to Q/K only. */
static void apply_rope(const nd_model *m, float *x, uint32_t nheads, uint32_t dim)
{
    uint32_t half = dim / 2;
    uint32_t h, i;

    for (h = 0; h < nheads; h++) {
        float *v = x + (size_t)h * dim;
        for (i = 0; i < half; i++) {
            float c  = m->rope_cos[i];
            float s  = m->rope_sin[i];
            float x1 = v[i];
            float x2 = v[i + half];
            v[i]        = x1 * c - x2 * s;
            v[i + half] = x2 * c + x1 * s;
        }
    }
}

/* Doubly-stochastic normalisation of a lanes x lanes matrix, in log space. */
static void sinkhorn(float *a, uint32_t n)
{
    uint32_t it, i, j;

    for (it = 0; it < ND_SINKHORN; it++) {
        for (i = 0; i < n; i++) {           /* rows */
            float mx = a[i * n];
            float sum = 0.0f;
            for (j = 1; j < n; j++)
                if (a[i * n + j] > mx) mx = a[i * n + j];
            for (j = 0; j < n; j++)
                sum += nd_expf(a[i * n + j] - mx);
            {
                float lse = mx + logf(sum);
                for (j = 0; j < n; j++)
                    a[i * n + j] -= lse;
            }
        }
        for (j = 0; j < n; j++) {           /* columns */
            float mx = a[j];
            float sum = 0.0f;
            for (i = 1; i < n; i++)
                if (a[i * n + j] > mx) mx = a[i * n + j];
            for (i = 0; i < n; i++)
                sum += nd_expf(a[i * n + j] - mx);
            {
                float lse = mx + logf(sum);
                for (i = 0; i < n; i++)
                    a[i * n + j] -= lse;
            }
        }
    }
    for (i = 0; i < n * n; i++)
        a[i] = nd_expf(a[i]);
}

static int layer_is_global(const nd_model *m, uint32_t li)
{
    if (li < 32)
        return (int)((m->c.h.global_mask_lo >> li) & 1u);
    if (li < 64)
        return (int)((m->c.h.global_mask_hi >> (li - 32)) & 1u);
    return 0;
}

static uint32_t layer_stride(const nd_model *m)
{
    return m->c.h.qkv_conv_taps ? 27u : 24u;
}

static uint32_t tap_hist(const nd_model *m)
{
    return (m->n_taps > 1) ? m->n_taps - 1 : 0;
}

/* Dequant every row of a CQ matrix into a dense float buffer, or expand FP16. */
static int expand_matrix(nd_model *m, const nd_tensor *t, float *out)
{
    uint32_t rows, cols, r, i;

    if (t->dtype == ND_DT_CQ) {
        rows = t->shape[0];
        cols = t->shape[1];
        for (r = 0; r < rows; r++)
            nd_cq_dequant_row(&m->c, t, nd_cact_data(&m->c, t), r,
                              m->xh, out + (size_t)r * cols);
        return 0;
    }
    if (t->dtype == ND_DT_FP16) {
        const uint16_t *p = (const uint16_t *)nd_cact_data(&m->c, t);
        rows = t->shape[0] ? t->shape[0] : 1;
        cols = t->shape[1] ? t->shape[1] : 1;
        if (t->ndim == 3)
            rows = t->shape[0] * t->shape[1], cols = t->shape[2];
        else if (t->ndim == 1)
            rows = 1, cols = t->shape[0];
        for (i = 0; i < rows * cols; i++)
            out[i] = nd_f16(p[i]);
        return 0;
    }
    return -1;
}

/* ------------------------------------------------------------------- open */

static void bind_layer(nd_model *m, uint32_t i)
{
    uint32_t  b = 1 + i * layer_stride(m);
    nd_layer *L = &m->layer[i];

    nd_cact_tensor(&m->c, b++, &L->norm_in);
    nd_cact_tensor(&m->c, b++, &L->q_proj);
    nd_cact_tensor(&m->c, b++, &L->k_proj);
    nd_cact_tensor(&m->c, b++, &L->v_proj);
    if (m->c.h.qkv_conv_taps) {
        nd_cact_tensor(&m->c, b++, &L->q_taps);
        nd_cact_tensor(&m->c, b++, &L->k_taps);
        nd_cact_tensor(&m->c, b++, &L->v_taps);
    }
    nd_cact_tensor(&m->c, b++, &L->q_norm);
    nd_cact_tensor(&m->c, b++, &L->k_norm);
    nd_cact_tensor(&m->c, b++, &L->gate_proj);
    nd_cact_tensor(&m->c, b++, &L->out_proj);
    nd_cact_tensor(&m->c, b++, &L->post_norm);
    nd_cact_tensor(&m->c, b++, &L->attn_gate);
    nd_cact_tensor(&m->c, b++, &L->pre_hada);
    nd_cact_tensor(&m->c, b++, &L->d1);
    nd_cact_tensor(&m->c, b++, &L->d2);
    nd_cact_tensor(&m->c, b++, &L->b2);
    nd_cact_tensor(&m->c, b++, &L->d3);
    nd_cact_tensor(&m->c, b++, &L->d4);
    nd_cact_tensor(&m->c, b++, &L->w1a);
    nd_cact_tensor(&m->c, b++, &L->w1b);
    nd_cact_tensor(&m->c, b++, &L->w2a);
    nd_cact_tensor(&m->c, b++, &L->w2b);
    nd_cact_tensor(&m->c, b++, &L->w3a);
    nd_cact_tensor(&m->c, b++, &L->w3b);
    nd_cact_tensor(&m->c, b++, &L->cond_v);
    nd_cact_tensor(&m->c, b++, &L->cond_u);
}

int nd_model_open(nd_model *m, const void *blob, size_t size)
{
    uint32_t base, s, i;
    int      rc;

    memset(m, 0, sizeof(*m));
    rc = nd_cact_open(&m->c, blob, size);
    if (rc != 0)
        return rc;

    m->d_model    = m->c.h.d_model;
    m->n_layers   = m->c.h.num_layers;
    m->n_heads    = m->c.h.num_heads;
    m->n_kv_heads = m->c.h.num_kv_heads;
    m->qk_hd      = m->c.h.qk_head_dim;
    m->v_hd       = m->c.h.v_head_dim;
    m->q_dim      = m->n_heads * m->qk_hd;
    m->k_dim      = m->n_kv_heads * m->qk_hd;
    m->v_dim      = m->n_kv_heads * m->v_hd;
    m->o_dim      = m->n_heads * m->v_hd;
    m->lanes      = m->c.h.mhc_lanes;
    m->vocab      = m->c.h.out_vocab ? m->c.h.out_vocab : m->c.h.vocab_size;
    m->window     = m->c.h.kv_window ? m->c.h.kv_window : m->c.h.max_seq_len;
    m->hada_n     = m->c.h.hada_n ? m->c.h.hada_n
                                  : (uint32_t)(1u << (32 - __builtin_clz(m->d_model - 1)));
    m->n_taps     = m->c.h.qkv_conv_taps;
    m->n_sites    = m->c.h.num_sites;

    if (m->lanes > ND_MAX_LANES || m->n_sites > ND_MAX_SITES)
        return -10;

    nd_cact_tensor(&m->c, 0, &m->embedding);

    m->layer = (nd_layer *)ND_ALLOC(sizeof(nd_layer) * m->n_layers);
    if (!m->layer)
        return -11;
    memset(m->layer, 0, sizeof(nd_layer) * m->n_layers);
    for (i = 0; i < m->n_layers; i++)
        bind_layer(m, i);

    base = 1 + m->n_layers * layer_stride(m);
    nd_cact_tensor(&m->c, base + 0, &m->mhc_a_pre);
    nd_cact_tensor(&m->c, base + 1, &m->mhc_a_post);
    nd_cact_tensor(&m->c, base + 2, &m->mhc_a_res);
    nd_cact_tensor(&m->c, base + 3, &m->mhc_b_pre);
    nd_cact_tensor(&m->c, base + 4, &m->mhc_b_post);
    nd_cact_tensor(&m->c, base + 5, &m->mhc_b_res);
    nd_cact_tensor(&m->c, base + 6, &m->mhc_phi_pre);
    nd_cact_tensor(&m->c, base + 7, &m->mhc_phi_post);
    nd_cact_tensor(&m->c, base + 8, &m->mhc_phi_res);
    nd_cact_tensor(&m->c, base + 9, &m->hada_p1);
    nd_cact_tensor(&m->c, base + 10, &m->hada_p2);
    base += 11;

    for (s = 0; s < m->n_sites; s++) {
        nd_cact_tensor(&m->c, base + s * 4 + 0, &m->engram[s].tables);
        nd_cact_tensor(&m->c, base + s * 4 + 1, &m->engram[s].key_proj);
        nd_cact_tensor(&m->c, base + s * 4 + 2, &m->engram[s].value_proj);
        nd_cact_tensor(&m->c, base + s * 4 + 3, &m->engram[s].taps);
    }
    base += m->n_sites * 4;
    nd_cact_tensor(&m->c, base, &m->final_norm);

    /* Probe heads after final_norm: heads.manifest, then each head's tensors
     * [probes, gain, query, row_bias, proj, bias] (+ calibration for router).
     * The tokenizer is the final RAW tensor. */
    {
        uint32_t after = base + 1;
        uint32_t extra = (m->c.n > after + 1) ? m->c.n - after - 1 : 0;

        if (extra >= 7) {
            nd_tensor manifest;
            uint32_t  heads, k, ti;

            nd_cact_tensor(&m->c, after, &manifest);
            heads = manifest.shape[0];
            ti = after + 1;
            for (k = 0; k < heads && ti + 5 < m->c.n; k++) {
                float code = fp16_get(m, &manifest, k);
                if (code > 1.5f && code < 2.5f) {
                    nd_tensor probes, query, proj;
                    nd_cact_tensor(&m->c, ti + 0, &probes);
                    nd_cact_tensor(&m->c, ti + 1, &m->conf_gain);
                    nd_cact_tensor(&m->c, ti + 2, &query);
                    nd_cact_tensor(&m->c, ti + 3, &m->conf_row_bias);
                    nd_cact_tensor(&m->c, ti + 4, &proj);
                    nd_cact_tensor(&m->c, ti + 5, &m->conf_bias);
                    m->n_cells = m->n_layers + 1;
                    if (probes.dtype == ND_DT_CQ)
                        m->n_k = probes.shape[0] / m->n_cells;
                    else if (probes.ndim == 3)
                        m->n_k = probes.shape[1];
                    else
                        m->n_k = probes.shape[0] / m->n_cells;
                    m->n_q = (query.dtype == ND_DT_CQ || query.ndim >= 2)
                                 ? query.shape[0] : 1;
                    if (m->n_k > 0 && m->n_q > 0 &&
                        m->n_k * m->n_cells > 0) {
                        m->has_conf = 1;
                        /* Keep the tensor records for expand_matrix below. */
                        m->probes_f = (float *)(uintptr_t)(ti + 0);
                        m->query_f  = (float *)(uintptr_t)(ti + 2);
                        m->proj_f   = (float *)(uintptr_t)(ti + 4);
                    }
                    ti += 6;
                } else if (code > 2.5f && code < 3.5f) {
                    ti += 7;
                } else {
                    ti += 6;
                }
            }
        }
    }

    for (i = m->c.n; i-- > 0;) {
        nd_tensor t;
        nd_cact_tensor(&m->c, i, &t);
        if (t.dtype == ND_DT_RAW) {
            if (nd_tok_init(&m->tok, nd_cact_data(&m->c, &t), (size_t)t.nbytes) == 0)
                m->tok_ready = 1;
            break;
        }
    }

    /* ---- buffers ---- */
    {
        uint32_t dm  = m->d_model;
        uint32_t nl  = m->lanes * dm;
        uint32_t hn  = m->hada_n;
        uint32_t th  = tap_hist(m);
        uint32_t in_max = nl > dm ? nl : dm;
        size_t   kn  = (size_t)m->n_layers * m->window * m->k_dim;
        size_t   vn  = (size_t)m->n_layers * m->window * m->v_dim;
        size_t   scn = (size_t)m->n_layers * m->window * m->n_kv_heads;

        m->lane      = (float *)ND_ALLOC_FAST(sizeof(float) * nl);
        m->lane_next = (float *)ND_ALLOC_FAST(sizeof(float) * nl);
        m->nx        = (float *)ND_ALLOC_FAST(sizeof(float) * nl);
        m->xh        = (float *)ND_ALLOC_FAST(sizeof(float) * in_max);
        m->lut       = (float *)ND_ALLOC_FAST(sizeof(float) * nd_cq_lut_floats(in_max));

        m->u         = (float *)ND_ALLOC_FAST(sizeof(float) * dm);
        m->ublk      = (float *)ND_ALLOC_FAST(sizeof(float) * dm);
        m->n1        = (float *)ND_ALLOC_FAST(sizeof(float) * (hn > dm ? hn : dm));
        m->n2        = (float *)ND_ALLOC_FAST(sizeof(float) * (hn > dm ? hn : dm));
        m->y         = (float *)ND_ALLOC_FAST(sizeof(float) * dm);
        m->tmp       = (float *)ND_ALLOC_FAST(sizeof(float) * dm);
        m->tmp2      = (float *)ND_ALLOC_FAST(sizeof(float) * (hn > dm ? hn : dm));
        m->hwork     = (float *)ND_ALLOC_FAST(sizeof(float) * (hn > dm ? hn : dm));
        m->q         = (float *)ND_ALLOC_FAST(sizeof(float) * m->q_dim);
        m->kbuf      = (float *)ND_ALLOC_FAST(sizeof(float) * m->k_dim);
        m->vbuf      = (float *)ND_ALLOC_FAST(sizeof(float) * m->v_dim);
        m->qraw      = (float *)ND_ALLOC_FAST(sizeof(float) * m->q_dim);
        m->kraw      = (float *)ND_ALLOC_FAST(sizeof(float) * m->k_dim);
        m->vraw      = (float *)ND_ALLOC_FAST(sizeof(float) * m->v_dim);
        m->gate      = (float *)ND_ALLOC_FAST(sizeof(float) * m->o_dim);
        m->attn      = (float *)ND_ALLOC_FAST(sizeof(float) * m->o_dim);
        m->aout      = (float *)ND_ALLOC_FAST(sizeof(float) * dm);
        m->rope_inv  = (float *)ND_ALLOC_FAST(sizeof(float) * (m->qk_hd / 2));
        m->rope_cos  = (float *)ND_ALLOC_FAST(sizeof(float) * (m->qk_hd / 2));
        m->rope_sin  = (float *)ND_ALLOC_FAST(sizeof(float) * (m->qk_hd / 2));
        m->eg_k      = (float *)ND_ALLOC(sizeof(float) * m->n_sites * dm);
        m->eg_v      = (float *)ND_ALLOC(sizeof(float) * m->n_sites * dm);
        m->eg_hist   = (float *)ND_ALLOC(sizeof(float) * m->n_sites * ND_EG_HIST * dm);
        m->logits    = (float *)ND_ALLOC(sizeof(float) * m->c.h.vocab_size);
        m->row       = (float *)ND_ALLOC_FAST(sizeof(float) * (dm > 128 ? dm : 128));
        m->k_cache   = (int8_t *)ND_ALLOC(kn);
        m->v_cache   = (int8_t *)ND_ALLOC(vn);
        m->k_scale   = (float *)ND_ALLOC(sizeof(float) * scn);
        m->v_scale   = (float *)ND_ALLOC(sizeof(float) * scn);

        if (th) {
            m->q_hist = (float *)ND_ALLOC(sizeof(float) * m->n_layers * th * m->q_dim);
            m->k_hist = (float *)ND_ALLOC(sizeof(float) * m->n_layers * th * m->k_dim);
            m->v_hist = (float *)ND_ALLOC(sizeof(float) * m->n_layers * th * m->v_dim);
        }

        if (m->has_conf) {
            uint32_t pk = m->n_cells * m->n_k;
            uint32_t p_ti = (uint32_t)(uintptr_t)m->probes_f;
            uint32_t q_ti = (uint32_t)(uintptr_t)m->query_f;
            uint32_t j_ti = (uint32_t)(uintptr_t)m->proj_f;
            nd_tensor probes, query, proj;

            m->probes_f = (float *)ND_ALLOC(sizeof(float) * pk * dm);
            m->query_f  = (float *)ND_ALLOC(sizeof(float) * m->n_q * dm);
            m->proj_f   = (float *)ND_ALLOC(sizeof(float) * m->n_q * dm);
            m->pool_acc = (float *)ND_ALLOC(sizeof(float) * pk * dm);
            m->pool_max = (float *)ND_ALLOC(sizeof(float) * pk);
            m->pool_sum = (float *)ND_ALLOC(sizeof(float) * pk);
            if (!m->probes_f || !m->query_f || !m->proj_f ||
                !m->pool_acc || !m->pool_max || !m->pool_sum) {
                nd_model_close(m);
                return -13;
            }
            nd_cact_tensor(&m->c, p_ti, &probes);
            nd_cact_tensor(&m->c, q_ti, &query);
            nd_cact_tensor(&m->c, j_ti, &proj);
            if (expand_matrix(m, &probes, m->probes_f) != 0 ||
                expand_matrix(m, &query, m->query_f) != 0 ||
                expand_matrix(m, &proj, m->proj_f) != 0) {
                nd_model_close(m);
                return -14;
            }
            m->conf_b = fp16_get(m, &m->conf_bias, 0);
        }

        if (!m->lane || !m->lane_next || !m->nx || !m->xh || !m->lut || !m->u || !m->ublk ||
            !m->n1 || !m->n2 || !m->y || !m->tmp || !m->tmp2 || !m->hwork || !m->q ||
            !m->kbuf || !m->vbuf || !m->qraw || !m->kraw || !m->vraw ||
            !m->gate || !m->attn || !m->aout ||
            !m->rope_inv || !m->rope_cos || !m->rope_sin ||
            !m->eg_k || !m->eg_v || !m->eg_hist || !m->logits ||
            !m->row || !m->k_cache || !m->v_cache || !m->k_scale || !m->v_scale ||
            (th && (!m->q_hist || !m->k_hist || !m->v_hist))) {
            nd_model_close(m);
            return -12;
        }
    }

    for (i = 0; i < m->qk_hd / 2; i++)
        m->rope_inv[i] = 1.0f / powf(m->c.h.rope_theta,
                                     (float)(2 * i) / (float)m->qk_hd);

    nd_model_reset(m);
    return 0;
}

void nd_model_close(nd_model *m)
{
    if (!m)
        return;
    if (m->tok_ready)
        nd_tok_free(&m->tok);
    ND_FREE(m->layer);
    ND_FREE(m->lane); ND_FREE(m->lane_next); ND_FREE(m->nx); ND_FREE(m->xh);
    ND_FREE(m->lut);
    ND_FREE(m->u); ND_FREE(m->ublk); ND_FREE(m->n1); ND_FREE(m->n2);
    ND_FREE(m->y); ND_FREE(m->tmp); ND_FREE(m->tmp2); ND_FREE(m->hwork);
    ND_FREE(m->q); ND_FREE(m->kbuf); ND_FREE(m->vbuf);
    ND_FREE(m->qraw); ND_FREE(m->kraw); ND_FREE(m->vraw);
    ND_FREE(m->gate); ND_FREE(m->attn); ND_FREE(m->aout);
    ND_FREE(m->rope_inv); ND_FREE(m->rope_cos); ND_FREE(m->rope_sin);
    ND_FREE(m->eg_k); ND_FREE(m->eg_v); ND_FREE(m->eg_hist);
    ND_FREE(m->logits); ND_FREE(m->row);
    ND_FREE(m->k_cache); ND_FREE(m->v_cache);
    ND_FREE(m->k_scale); ND_FREE(m->v_scale);
    ND_FREE(m->q_hist); ND_FREE(m->k_hist); ND_FREE(m->v_hist);
    ND_FREE(m->snap_eg_hist);
    ND_FREE(m->snap_pool_acc); ND_FREE(m->snap_pool_max); ND_FREE(m->snap_pool_sum);
    ND_FREE(m->snap_q_hist); ND_FREE(m->snap_k_hist); ND_FREE(m->snap_v_hist);
    ND_FREE(m->probes_f); ND_FREE(m->query_f); ND_FREE(m->proj_f);
    ND_FREE(m->pool_acc); ND_FREE(m->pool_max); ND_FREE(m->pool_sum);
    memset(m, 0, sizeof(*m));
}

/* Cache slot for an absolute position. Sinks occupy the first n_sink slots
 * permanently; everything after rings through the remainder. */
static uint32_t kv_slot(const nd_model *m, uint32_t p)
{
    if (p < m->n_sink)
        return p;
    return m->n_sink + (p - m->n_sink) % (m->window - m->n_sink);
}

#define ND_MIN_RECENT 64

void nd_model_set_sink(nd_model *m, uint32_t n)
{
    uint32_t cap = (m->window > ND_MIN_RECENT) ? m->window - ND_MIN_RECENT : 0;
    m->n_sink = (n > cap) ? cap : n;
}

static uint32_t pool_n(const nd_model *m)
{
    return m->n_cells * m->n_k;
}

static void pool_cell(nd_model *m, uint32_t layer, const float *cell)
{
    const float *probes = m->probes_f;
    uint32_t     dm     = m->d_model;
    float        inv_s  = 1.0f / sqrtf((float)dm);
    uint32_t     k, i;

    for (k = 0; k < m->n_k; k++) {
        uint32_t     idx = layer * m->n_k + k;
        const float *pr  = probes + (size_t)idx * dm;
        float       *ac  = m->pool_acc + (size_t)idx * dm;
        float        z = 0.0f, w;

        for (i = 0; i < dm; i++)
            z += pr[i] * cell[i];
        z *= inv_s;

        if (z > m->pool_max[idx]) {
            float rescale = expf(m->pool_max[idx] - z);
            for (i = 0; i < dm; i++)
                ac[i] *= rescale;
            m->pool_sum[idx] *= rescale;
            m->pool_max[idx] = z;
        }
        w = expf(z - m->pool_max[idx]);
        m->pool_sum[idx] += w;
        for (i = 0; i < dm; i++)
            ac[i] += w * cell[i];
    }
}

float nd_model_confidence(nd_model *m)
{
    uint32_t dm, pk, q, lk, i;
    float    inv_s, logit;
    float    u[256];

    if (!m->has_conf)
        return -1.0f;

    dm    = m->d_model;
    pk    = pool_n(m);
    inv_s = 1.0f / sqrtf((float)dm);

    /* Stage 1 finish: r[l,k] = rms(mean_t) * gain. In-place on pool_acc. */
    for (lk = 0; lk < pk; lk++) {
        float *ac  = m->pool_acc + (size_t)lk * dm;
        float  inv = (m->pool_sum[lk] > 0.0f) ? 1.0f / m->pool_sum[lk] : 0.0f;
        float  gain, ss = 0.0f, rinv;
        uint32_t layer = lk / m->n_k, kk = lk % m->n_k;
        for (i = 0; i < dm; i++) {
            ac[i] *= inv;
            ss += ac[i] * ac[i];
        }
        rinv = 1.0f / sqrtf(ss / (float)dm + ND_EPS);
        gain = fp16_get(m, &m->conf_gain, (size_t)layer * m->n_k + kk);
        for (i = 0; i < dm; i++)
            ac[i] *= rinv * gain;
    }

    /* Stage 2: attend over (layer, probe) with n_q queries, then project. */
    logit = m->conf_b;
    for (q = 0; q < m->n_q; q++) {
        const float *qr = m->query_f + (size_t)q * dm;
        float        mx, sum = 0.0f;
        for (lk = 0; lk < pk; lk++) {
            const float *r = m->pool_acc + (size_t)lk * dm;
            float        z = 0.0f;
            uint32_t     layer = lk / m->n_k, kk = lk % m->n_k;
            for (i = 0; i < dm; i++)
                z += r[i] * qr[i];
            u[lk] = z * inv_s + fp16_get(m, &m->conf_row_bias,
                                         ((size_t)q * m->n_cells + layer) * m->n_k + kk);
        }
        mx = u[0];
        for (lk = 1; lk < pk; lk++)
            if (u[lk] > mx) mx = u[lk];
        for (lk = 0; lk < pk; lk++) {
            u[lk] = nd_expf(u[lk] - mx);
            sum += u[lk];
        }
        {
            float inv = (sum > 0.0f) ? 1.0f / sum : 0.0f;
            for (i = 0; i < dm; i++) {
                float acc = 0.0f;
                for (lk = 0; lk < pk; lk++)
                    acc += u[lk] * inv * m->pool_acc[(size_t)lk * dm + i];
                logit += m->proj_f[(size_t)q * dm + i] * acc;
            }
        }
    }
    return sigmoidf_(logit);
}

static size_t qhist_n(const nd_model *m)
{
    return (size_t)m->n_layers * tap_hist(m) * m->q_dim;
}
static size_t khist_n(const nd_model *m)
{
    return (size_t)m->n_layers * tap_hist(m) * m->k_dim;
}
static size_t vhist_n(const nd_model *m)
{
    return (size_t)m->n_layers * tap_hist(m) * m->v_dim;
}

int nd_model_snapshot(nd_model *m)
{
    size_t egn = (size_t)m->n_sites * ND_EG_HIST * m->d_model;
    size_t qn = qhist_n(m), kn = khist_n(m), vn = vhist_n(m);

    if (!m->snap_eg_hist) {
        m->snap_eg_hist = (float *)ND_ALLOC(sizeof(float) * egn);
        if (!m->snap_eg_hist)
            return -1;
        if (m->has_conf) {
            uint32_t pk = pool_n(m);
            m->snap_pool_acc = (float *)ND_ALLOC(sizeof(float) * pk * m->d_model);
            m->snap_pool_max = (float *)ND_ALLOC(sizeof(float) * pk);
            m->snap_pool_sum = (float *)ND_ALLOC(sizeof(float) * pk);
            if (!m->snap_pool_acc || !m->snap_pool_max || !m->snap_pool_sum)
                return -1;
        }
        if (qn) {
            m->snap_q_hist = (float *)ND_ALLOC(sizeof(float) * qn);
            m->snap_k_hist = (float *)ND_ALLOC(sizeof(float) * kn);
            m->snap_v_hist = (float *)ND_ALLOC(sizeof(float) * vn);
            if (!m->snap_q_hist || !m->snap_k_hist || !m->snap_v_hist)
                return -1;
        }
    }

    nd_model_set_sink(m, m->pos);
    m->snap_pos    = m->pos;
    m->snap_eg_pos = m->eg_pos;
    memcpy(m->snap_hist, m->hist, sizeof(m->hist));
    memcpy(m->snap_eg_hist, m->eg_hist, sizeof(float) * egn);
    if (m->has_conf) {
        uint32_t pk = pool_n(m);
        memcpy(m->snap_pool_acc, m->pool_acc, sizeof(float) * pk * m->d_model);
        memcpy(m->snap_pool_max, m->pool_max, sizeof(float) * pk);
        memcpy(m->snap_pool_sum, m->pool_sum, sizeof(float) * pk);
    }
    if (qn) {
        memcpy(m->snap_q_hist, m->q_hist, sizeof(float) * qn);
        memcpy(m->snap_k_hist, m->k_hist, sizeof(float) * kn);
        memcpy(m->snap_v_hist, m->v_hist, sizeof(float) * vn);
    }
    m->has_snap = 1;
    return 0;
}

void nd_model_rewind(nd_model *m)
{
    size_t egn = (size_t)m->n_sites * ND_EG_HIST * m->d_model;

    if (!m->has_snap) {
        nd_model_reset(m);
        return;
    }
    m->pos    = m->snap_pos;
    m->eg_pos = m->snap_eg_pos;
    memcpy(m->hist, m->snap_hist, sizeof(m->hist));
    memcpy(m->eg_hist, m->snap_eg_hist, sizeof(float) * egn);
    if (m->has_conf) {
        uint32_t pk = pool_n(m);
        memcpy(m->pool_acc, m->snap_pool_acc, sizeof(float) * pk * m->d_model);
        memcpy(m->pool_max, m->snap_pool_max, sizeof(float) * pk);
        memcpy(m->pool_sum, m->snap_pool_sum, sizeof(float) * pk);
    }
    if (m->q_hist && m->snap_q_hist) {
        memcpy(m->q_hist, m->snap_q_hist, sizeof(float) * qhist_n(m));
        memcpy(m->k_hist, m->snap_k_hist, sizeof(float) * khist_n(m));
        memcpy(m->v_hist, m->snap_v_hist, sizeof(float) * vhist_n(m));
    }
}

void nd_model_reset(nd_model *m)
{
    m->pos    = 0;
    m->n_sink = 0;
    m->has_snap = 0;
    m->eg_pos = 0;
    if (m->has_conf) {
        uint32_t k, pk = pool_n(m);
        memset(m->pool_acc, 0, sizeof(float) * pk * m->d_model);
        for (k = 0; k < pk; k++) {
            m->pool_max[k] = -INFINITY;
            m->pool_sum[k] = 0.0f;
        }
    }
    memset(m->hist, 0, sizeof(m->hist));
    if (m->eg_hist)
        memset(m->eg_hist, 0,
               sizeof(float) * m->n_sites * ND_EG_HIST * m->d_model);
    if (m->q_hist) {
        memset(m->q_hist, 0, sizeof(float) * qhist_n(m));
        memset(m->k_hist, 0, sizeof(float) * khist_n(m));
        memset(m->v_hist, 0, sizeof(float) * vhist_n(m));
    }
}

/* ----------------------------------------------------------------- engram */

static void engram_step(nd_model *m, uint32_t token)
{
    uint32_t orders = m->c.h.num_orders;
    uint32_t heads  = m->c.h.engram_tables / (orders ? orders : 1);
    uint32_t slots  = m->c.h.engram_slots;
    uint32_t sub    = m->c.h.engram_sub_dim;
    uint32_t dil    = m->c.h.engram_dilation;
    uint32_t taps   = m->c.h.engram_conv_taps;
    uint32_t stride = m->c.h.engram_seed_heads ? m->c.h.engram_seed_heads : heads;
    uint32_t dm     = m->d_model;
    uint32_t s, oi, h, j;

    for (j = 7; j > 0; j--)
        m->hist[j] = m->hist[j - 1];
    m->hist[0] = token;

    for (s = 0; s < m->n_sites; s++) {
        float   *e = m->xh;
        uint32_t table = 0;

        for (oi = 0; oi < orders; oi++) {
            uint32_t order = m->c.h.orders[oi];
            for (h = 0; h < heads; h++, table++) {
                uint32_t seed = ND_EG_SEED * (oi * stride + h + 1);
                uint32_t acc  = seed;
                uint32_t idx;
                int      ok   = (m->pos + 1 >= order);

                for (j = 0; j < order; j++) {
                    uint32_t tk = (j <= m->pos) ? m->hist[j] : 0u;
                    acc = (acc ^ tk) * ND_EG_PRIME;
                }
                acc ^= acc >> 15;
                idx = acc % slots;

                if (ok) {
                    nd_tensor *tt = &m->engram[s].tables;
                    nd_cq_dequant_row(&m->c, tt, nd_cact_data(&m->c, tt),
                                      table * slots + idx, m->row,
                                      e + (size_t)table * sub);
                } else {
                    memset(e + (size_t)table * sub, 0, sizeof(float) * sub);
                }
            }
        }

        {
            nd_tensor *kp = &m->engram[s].key_proj;
            nd_tensor *vp = &m->engram[s].value_proj;
            float     *vraw = m->eg_hist + ((size_t)s * ND_EG_HIST +
                                            (m->eg_pos % ND_EG_HIST)) * dm;
            nd_cq_prepare(kp, e, m->tmp2);
            nd_cq_lut_build(&m->c, m->tmp2, nd_cq_in_pad(kp), m->lut);
            nd_cq_gemv_lut2(kp, nd_cact_data(&m->c, kp), m->lut,
                            m->eg_k + (size_t)s * dm);
            nd_cq_gemv_lut2(vp, nd_cact_data(&m->c, vp), m->lut, vraw);
        }

        {
            const uint16_t *tp = (const uint16_t *)nd_cact_data(&m->c,
                                                               &m->engram[s].taps);
            float *out = m->eg_v + (size_t)s * dm;
            uint32_t d;

            memset(out, 0, sizeof(float) * dm);
            for (j = 0; j < taps; j++) {
                uint32_t back = j * dil;
                const float *src;
                if (back > m->pos)
                    continue;
                src = m->eg_hist + ((size_t)s * ND_EG_HIST +
                                    ((m->eg_pos - back) % ND_EG_HIST)) * dm;
                for (d = 0; d < dm; d++)
                    out[d] += nd_f16(tp[j * dm + d]) * src[d];
            }
        }
    }
    m->eg_pos++;
}

/* ------------------------------------------------------------- attention */

typedef struct {
    nd_model *m;
    uint32_t  li, nkv, rep, qk, vd, sinks, rfirst, rcount;
    float     scale;
} attn_ctx;

static ND_HOT void attn_heads(void *vc, uint32_t h0, uint32_t h1)
{
    const attn_ctx *c   = (const attn_ctx *)vc;
    nd_model       *m   = c->m;
    uint32_t        qk  = c->qk;
    uint32_t        vd  = c->vd;
    uint32_t        nkv = c->nkv;
    uint32_t        li  = c->li;
    uint32_t        h, i;

    for (h = h0; h < h1; h++) {
        const float *qh  = m->q + (size_t)h * qk;
        float       *oh  = m->attn + (size_t)h * vd;
        uint32_t     kvh = h / c->rep;
        float        mx = -INFINITY, denom = 0.0f;
        uint32_t     run, p;

        memset(oh, 0, sizeof(float) * vd);

        for (run = 0; run < 2; run++) {
            uint32_t base  = run ? c->rfirst : 0;
            uint32_t count = run ? c->rcount : c->sinks;

            for (p = 0; p < count; p++) {
                uint32_t      sl = kv_slot(m, base + p);
                size_t        kb = ((size_t)li * m->window + sl) * m->k_dim
                                   + (size_t)kvh * qk;
                size_t        vb = ((size_t)li * m->window + sl) * m->v_dim
                                   + (size_t)kvh * vd;
                size_t        sb = ((size_t)li * m->window + sl) * nkv + kvh;
                const int8_t *kp = m->k_cache + kb;
                const int8_t *vp = m->v_cache + vb;
                float         dot = 0.0f, w;

                for (i = 0; i < qk; i++)
                    dot += qh[i] * (float)kp[i];
                dot *= m->k_scale[sb] * c->scale;

                if (dot > mx) {
                    if (denom > 0.0f) {
                        float rescale = nd_expf(mx - dot);
                        for (i = 0; i < vd; i++)
                            oh[i] *= rescale;
                        denom *= rescale;
                    }
                    mx = dot;
                }
                w = nd_expf(dot - mx);
                denom += w;
                {
                    float wv = w * m->v_scale[sb];
                    for (i = 0; i < vd; i++)
                        oh[i] += wv * (float)vp[i];
                }
            }
        }
        {
            float inv = 1.0f / denom;
            for (i = 0; i < vd; i++)
                oh[i] *= inv;
        }
    }
}

/* Depthwise causal conv over the last n_taps raw projections. */
static void apply_taps(nd_model *m, uint32_t li, const nd_tensor *taps,
                       float *cur, float *hist, uint32_t dim, float *out)
{
    const uint16_t *tp = (const uint16_t *)nd_cact_data(&m->c, taps);
    uint32_t        nt = m->n_taps;
    uint32_t        th = tap_hist(m);
    uint32_t        j, i;

    memset(out, 0, sizeof(float) * dim);
    for (j = 0; j < nt; j++) {
        const float *src;
        if (j > m->pos)
            continue;
        src = (j == 0) ? cur
                       : hist + ((size_t)li * th + (j - 1)) * dim;
        for (i = 0; i < dim; i++)
            out[i] += nd_f16(tp[j * dim + i]) * src[i];
    }
    if (th) {
        for (j = th; j-- > 1;)
            memcpy(hist + ((size_t)li * th + j) * dim,
                   hist + ((size_t)li * th + (j - 1)) * dim,
                   sizeof(float) * dim);
        memcpy(hist + (size_t)li * th * dim, cur, sizeof(float) * dim);
    }
}

static void attention(nd_model *m, uint32_t li, const float *xin, float *out)
{
    const nd_layer *L    = &m->layer[li];
    uint32_t        qk   = m->qk_hd;
    uint32_t        vd   = m->v_hd;
    uint32_t        nh   = m->n_heads;
    uint32_t        nkv  = m->n_kv_heads;
    uint32_t        slot = kv_slot(m, m->pos);
    size_t          kbase = ((size_t)li * m->window + slot) * m->k_dim;
    size_t          vbase = ((size_t)li * m->window + slot) * m->v_dim;
    size_t          scbase = ((size_t)li * m->window + slot) * nkv;
    uint32_t        i, kh;

    { ND_T0(tp);
      nd_cq_prepare(&L->q_proj, xin, m->xh);
      nd_cq_lut_build(&m->c, m->xh, nd_cq_in_pad(&L->q_proj), m->lut);
      ND_T1(tp, ND_P_PREP); }
    { ND_T0(tg);
      nd_cq_gemv_lut2(&L->q_proj,    nd_cact_data(&m->c, &L->q_proj),    m->lut, m->n_taps ? m->qraw : m->q);
      nd_cq_gemv_lut2(&L->k_proj,    nd_cact_data(&m->c, &L->k_proj),    m->lut, m->n_taps ? m->kraw : m->kbuf);
      nd_cq_gemv_lut2(&L->v_proj,    nd_cact_data(&m->c, &L->v_proj),    m->lut, m->n_taps ? m->vraw : m->vbuf);
      nd_cq_gemv_lut2(&L->gate_proj, nd_cact_data(&m->c, &L->gate_proj), m->lut, m->gate);
      ND_T1(tg, ND_P_PROJ); }

    if (m->n_taps) {
        apply_taps(m, li, &L->q_taps, m->qraw, m->q_hist, m->q_dim, m->q);
        apply_taps(m, li, &L->k_taps, m->kraw, m->k_hist, m->k_dim, m->kbuf);
        apply_taps(m, li, &L->v_taps, m->vraw, m->v_hist, m->v_dim, m->vbuf);
    }

    zcrms_heads(m, &L->q_norm, m->q, nh, qk);
    zcrms_heads(m, &L->k_norm, m->kbuf, nkv, qk);

    apply_rope(m, m->q, nh, qk);
    apply_rope(m, m->kbuf, nkv, qk);

    for (kh = 0; kh < nkv; kh++) {
        float mk = 0.0f, mv = 0.0f;
        for (i = 0; i < qk; i++) {
            float a = fabsf(m->kbuf[kh * qk + i]);
            if (a > mk) mk = a;
        }
        for (i = 0; i < vd; i++) {
            float b = fabsf(m->vbuf[kh * vd + i]);
            if (b > mv) mv = b;
        }
        {
            float ks = (mk > 0.0f) ? mk / 127.0f : 1.0f;
            float vs = (mv > 0.0f) ? mv / 127.0f : 1.0f;
            m->k_scale[scbase + kh] = ks;
            m->v_scale[scbase + kh] = vs;
            for (i = 0; i < qk; i++) {
                float kq = m->kbuf[kh * qk + i] / ks;
                if (kq > 127.0f)  kq = 127.0f;
                if (kq < -127.0f) kq = -127.0f;
                m->k_cache[kbase + kh * qk + i] = (int8_t)lrintf(kq);
            }
            for (i = 0; i < vd; i++) {
                float vq = m->vbuf[kh * vd + i] / vs;
                if (vq > 127.0f)  vq = 127.0f;
                if (vq < -127.0f) vq = -127.0f;
                m->v_cache[vbase + kh * vd + i] = (int8_t)lrintf(vq);
            }
        }
    }

    { ND_T0(ta);
    {
        attn_ctx actx;
        uint32_t look = m->window;
        uint32_t rcap;

        if (m->c.h.sliding_window && !layer_is_global(m, li) &&
            m->c.h.sliding_window < look)
            look = m->c.h.sliding_window;
        rcap = look > m->n_sink ? look - m->n_sink : 0;

        actx.m      = m;
        actx.li     = li;
        actx.nkv    = nkv;
        actx.rep    = nh / nkv;
        actx.qk     = qk;
        actx.vd     = vd;
        actx.scale  = 1.0f / sqrtf((float)qk);
        actx.sinks  = (m->n_sink < m->pos + 1) ? m->n_sink : m->pos + 1;
        if (m->c.h.sliding_window && !layer_is_global(m, li)) {
            uint32_t oldest = (m->pos + 1 > look) ? m->pos + 1 - look : 0;
            if (actx.sinks <= oldest)
                actx.sinks = 0;
        }

        if (m->pos + 1 <= m->n_sink) {
            actx.rfirst = m->pos + 1;
            actx.rcount = 0;
        } else {
            actx.rcount = m->pos + 1 - m->n_sink;
            if (actx.rcount > rcap)
                actx.rcount = rcap;
            actx.rfirst = m->pos + 1 - actx.rcount;
        }
        nd_parallel_rows(attn_heads, &actx, nh);
    }
    ND_T1(ta, ND_P_ATTN); }

    for (i = 0; i < m->o_dim; i++)
        m->attn[i] *= sigmoidf_(m->gate[i]);

    { ND_T0(tp2);
      nd_cq_prepare(&L->out_proj, m->attn, m->xh);
      nd_cq_lut_build(&m->c, m->xh, nd_cq_in_pad(&L->out_proj), m->lut);
      ND_T1(tp2, ND_P_PREP); }
    { ND_T0(tg2);
      nd_cq_gemv_lut2(&L->out_proj, nd_cact_data(&m->c, &L->out_proj), m->lut, out);
      ND_T1(tg2, ND_P_PROJ); }
}

/* -------------------------------------------------------- Hadamard MLP */

/* out[k,l] = sum_ij z[i,j] * a[i,k] * b[j,l] */
static void kron_apply(const float *z, const uint16_t *a, const uint16_t *b,
                       uint32_t ba, uint32_t bb, float *out, float *tmp)
{
    uint32_t i, j, k, l;

    for (k = 0; k < ba; k++) {
        for (j = 0; j < bb; j++) {
            float acc = 0.0f;
            for (i = 0; i < ba; i++)
                acc += z[i * bb + j] * nd_f16(a[i * ba + k]);
            tmp[k * bb + j] = acc;
        }
    }
    for (k = 0; k < ba; k++) {
        for (l = 0; l < bb; l++) {
            float acc = 0.0f;
            for (j = 0; j < bb; j++)
                acc += tmp[k * bb + j] * nd_f16(b[j * bb + l]);
            out[k * bb + l] = acc;
        }
    }
}

static void hada_blocks(uint32_t n, uint32_t *ba, uint32_t *bb)
{
    uint32_t bits = 0, m = n - 1, b;
    while (m) {
        bits++;
        m >>= 1;
    }
    b = 1u << (bits / 2);
    *ba = b;
    *bb = n / b;
}

static void permute_from(const float *src, const nd_model *m, const nd_tensor *p,
                         uint32_t n, float *dst)
{
    const float *idx = (const float *)nd_cact_data(&m->c, p);
    uint32_t     i;
    for (i = 0; i < n; i++)
        dst[i] = src[(uint32_t)idx[i]];
}

static void hadamard_mlp(nd_model *m, uint32_t li, const float *x, float *out)
{
    const nd_layer *L  = &m->layer[li];
    uint32_t        dm = m->d_model;
    uint32_t        n  = m->hada_n;
    uint32_t        ba, bb, i, r;
    const uint16_t *d1, *d2, *b2, *d3, *d4, *cv, *cu;
    float           clog[ND_HADA_COND], mx, sm, cond_i;

    hada_blocks(n, &ba, &bb);

    d1 = (const uint16_t *)nd_cact_data(&m->c, &L->d1);
    d2 = (const uint16_t *)nd_cact_data(&m->c, &L->d2);
    b2 = (const uint16_t *)nd_cact_data(&m->c, &L->b2);
    d3 = (const uint16_t *)nd_cact_data(&m->c, &L->d3);
    d4 = (const uint16_t *)nd_cact_data(&m->c, &L->d4);
    cv = (const uint16_t *)nd_cact_data(&m->c, &L->cond_v);
    cu = (const uint16_t *)nd_cact_data(&m->c, &L->cond_u);

    /* cond = 1 + softmax(x @ cond_v) @ cond_u */
    mx = -INFINITY;
    for (r = 0; r < ND_HADA_COND; r++) {
        float acc = 0.0f;
        for (i = 0; i < dm; i++)
            acc += x[i] * nd_f16(cv[i * ND_HADA_COND + r]);
        clog[r] = acc;
        if (acc > mx) mx = acc;
    }
    sm = 0.0f;
    for (r = 0; r < ND_HADA_COND; r++) {
        clog[r] = nd_expf(clog[r] - mx);
        sm += clog[r];
    }
    {
        float inv = 1.0f / sm;
        for (r = 0; r < ND_HADA_COND; r++)
            clog[r] *= inv;
    }

    for (i = 0; i < dm; i++)
        m->n2[i] = nd_f16(d1[i]) * x[i];
    for (i = dm; i < n; i++)
        m->n2[i] = 0.0f;

    kron_apply(m->n2,
               (const uint16_t *)nd_cact_data(&m->c, &L->w1a),
               (const uint16_t *)nd_cact_data(&m->c, &L->w1b),
               ba, bb, m->hwork, m->tmp2);
    permute_from(m->hwork, m, &m->hada_p1, n, m->n2);

    for (i = 0; i < n; i++) {
        cond_i = 1.0f;
        for (r = 0; r < ND_HADA_COND; r++)
            cond_i += clog[r] * nd_f16(cu[r * n + i]);
        {
            float z = nd_f16(d2[i]) * cond_i * m->n2[i] + nd_f16(b2[i]);
            m->n2[i] = z * sigmoidf_(z);
        }
    }

    kron_apply(m->n2,
               (const uint16_t *)nd_cact_data(&m->c, &L->w2a),
               (const uint16_t *)nd_cact_data(&m->c, &L->w2b),
               ba, bb, m->hwork, m->tmp2);
    permute_from(m->hwork, m, &m->hada_p2, n, m->n2);

    for (i = 0; i < n; i++)
        m->n2[i] *= nd_f16(d3[i]);

    kron_apply(m->n2,
               (const uint16_t *)nd_cact_data(&m->c, &L->w3a),
               (const uint16_t *)nd_cact_data(&m->c, &L->w3b),
               ba, bb, m->hwork, m->tmp2);

    for (i = 0; i < dm; i++)
        out[i] = nd_f16(d4[i]) * m->hwork[i];
}

/* ------------------------------------------------------------------ block */

static void block(nd_model *m, uint32_t li, float *u)
{
    const nd_layer *L  = &m->layer[li];
    uint32_t        dm = m->d_model;
    uint32_t        s, i;

    for (s = 0; s < m->n_sites; s++) {
        if (m->c.h.sites[s] != li)
            continue;
        {
            const float *ek = m->eg_k + (size_t)s * dm;
            const float *ev = m->eg_v + (size_t)s * dm;
            float        dot = 0.0f, alpha;

            rms_unit(u, dm, m->n1);
            rms_unit(ek, dm, m->n2);
            for (i = 0; i < dm; i++)
                dot += m->n1[i] * m->n2[i];
            alpha = sigmoidf_(dot / sqrtf((float)dm));
            for (i = 0; i < dm; i++)
                u[i] += alpha * ev[i];
        }
    }

    zcrms(m, &L->norm_in, u, dm, m->n1);
    attention(m, li, m->n1, m->aout);
    zcrms(m, &L->post_norm, m->aout, dm, m->n2);
    {
        float g = sigmoidf_(fp16_get(m, &L->attn_gate, 0));
        for (i = 0; i < dm; i++)
            u[i] += g * m->n2[i];
    }

    { ND_T0(tm);
    zcrms(m, &L->pre_hada, u, dm, m->n1);
    hadamard_mlp(m, li, m->n1, m->n2);
    ND_T1(tm, ND_P_MLP); }
    for (i = 0; i < dm; i++)
        u[i] += m->n2[i];
}

/* ------------------------------------------------------------------- step */

const float *nd_model_step(nd_model *m, uint32_t token)
{
    const float *hidden = nd_model_step_hidden(m, token);
    return nd_model_logits_all(m, hidden);
}

const float *nd_model_logits_all(nd_model *m, const float *hidden)
{
    ND_T0(tl);
    if (m->vocab < m->c.h.vocab_size) {
        nd_cq_prepare(&m->embedding, hidden, m->xh);
        nd_cq_gemv_rows(&m->c, &m->embedding, nd_cact_data(&m->c, &m->embedding),
                        m->xh, 0, m->vocab, m->logits);
    } else {
        nd_cq_gemv(&m->c, &m->embedding, nd_cact_data(&m->c, &m->embedding),
                   hidden, m->xh, m->logits);
    }
    ND_T1(tl, ND_P_LOGITS);
    return m->logits;
}

void nd_model_logits_subset(nd_model *m, const float *hidden,
                            const uint32_t *ids, uint32_t n, float *out)
{
    ND_T0(tl);
    nd_cq_prepare(&m->embedding, hidden, m->xh);
    nd_cq_gemv_gather(&m->c, &m->embedding, nd_cact_data(&m->c, &m->embedding),
                      m->xh, ids, n, out);
    ND_T1(tl, ND_P_LOGITS);
}

const float *nd_model_step_hidden(nd_model *m, uint32_t token)
{
    uint32_t dm    = m->d_model;
    uint32_t n     = m->lanes;
    uint32_t nl    = n * dm;
    float    escale = sqrtf((float)dm);
    uint32_t i, j, k, li;

    for (i = 0; i < m->qk_hd / 2; i++) {
        float angle = (float)m->pos * m->rope_inv[i];
        m->rope_cos[i] = cosf(angle);
        m->rope_sin[i] = sinf(angle);
    }

    nd_cq_dequant_row(&m->c, &m->embedding, nd_cact_data(&m->c, &m->embedding),
                      token, m->xh, m->tmp);
    for (i = 0; i < dm; i++)
        m->tmp[i] *= escale;
    if (m->has_conf)
        pool_cell(m, 0, m->tmp);
    for (j = 0; j < n; j++)
        for (i = 0; i < dm; i++)
            m->lane[j * dm + i] = m->tmp[i];

    { ND_T0(te); engram_step(m, token); ND_T1(te, ND_P_ENGRAM); }

    for (li = 0; li < m->n_layers; li++) {
        float hpre[ND_MAX_LANES], hpost[ND_MAX_LANES];
        float hres[ND_MAX_LANES * ND_MAX_LANES];
        float a_pre  = fp16_get(m, &m->mhc_a_pre, li);
        float a_post = fp16_get(m, &m->mhc_a_post, li);
        float a_res  = fp16_get(m, &m->mhc_a_res, li);
        uint32_t lane_id = li % n;

        rms_unit(m->lane, nl, m->nx);

        ND_T0(tphi);
        nd_cq_prepare(&m->mhc_phi_pre, m->nx, m->xh);
        nd_cq_gemv_rows(&m->c, &m->mhc_phi_pre,
                        nd_cact_data(&m->c, &m->mhc_phi_pre), m->xh,
                        li * n, n, hpre);
        nd_cq_gemv_rows(&m->c, &m->mhc_phi_post,
                        nd_cact_data(&m->c, &m->mhc_phi_post), m->xh,
                        li * n, n, hpost);
        nd_cq_gemv_rows(&m->c, &m->mhc_phi_res,
                        nd_cact_data(&m->c, &m->mhc_phi_res), m->xh,
                        li * n * n, n * n, hres);
        ND_T1(tphi, ND_P_PHI);

        for (j = 0; j < n; j++) {
            float pre_off  = 8.0f * (j == lane_id ? 1.0f : 0.0f) - 4.0f;
            float post_off = -4.0f * (j == lane_id ? 0.0f : 1.0f);
            hpre[j]  = sigmoidf_(a_pre * hpre[j] +
                                 fp16_get(m, &m->mhc_b_pre, li * n + j) + pre_off);
            hpost[j] = 2.0f * sigmoidf_(a_post * hpost[j] +
                                 fp16_get(m, &m->mhc_b_post, li * n + j) + post_off);
        }
        for (i = 0; i < n * n; i++)
            hres[i] = a_res * hres[i] + fp16_get(m, &m->mhc_b_res, li * n * n + i);
        sinkhorn(hres, n);

        for (i = 0; i < dm; i++) {
            float acc = 0.0f;
            for (j = 0; j < n; j++)
                acc += hpre[j] * m->lane[j * dm + i];
            m->u[i] = acc;
        }

        memcpy(m->ublk, m->u, sizeof(float) * dm);
        block(m, li, m->u);
        for (i = 0; i < dm; i++)
            m->u[i] -= m->ublk[i];

        for (j = 0; j < n; j++) {
            float *dst = m->lane_next + (size_t)j * dm;
            for (i = 0; i < dm; i++) {
                float acc = 0.0f;
                for (k = 0; k < n; k++)
                    acc += hres[j * n + k] * m->lane[k * dm + i];
                dst[i] = acc + hpost[j] * m->u[i];
            }
        }
        {
            float *swap = m->lane;
            m->lane      = m->lane_next;
            m->lane_next = swap;
        }

        if (m->has_conf) {
            for (i = 0; i < dm; i++) {
                float acc = 0.0f;
                for (j = 0; j < n; j++)
                    acc += m->lane[j * dm + i];
                m->n1[i] = acc / (float)n;
            }
            { ND_T0(tc); pool_cell(m, li + 1, m->n1); ND_T1(tc, ND_P_CONF); }
        }
    }

    for (i = 0; i < dm; i++) {
        float acc = 0.0f;
        for (j = 0; j < n; j++)
            acc += m->lane[j * dm + i];
        m->tmp[i] = acc / (float)n;
    }
    zcrms(m, &m->final_norm, m->tmp, dm, m->y);

    m->pos++;
    return m->y;
}
