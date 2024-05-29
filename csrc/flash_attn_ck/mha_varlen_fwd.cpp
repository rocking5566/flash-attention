/******************************************************************************
 * Copyright (c) 2024, Tri Dao.
 ******************************************************************************/

#include "flash_common.hpp"

#include "fmha_fwd.hpp"
#include "mask.hpp"

fmha_fwd_traits get_ck_fmha_varlen_fwd_traits(const mask_info &mask,
                                              std::string dtype,
                                              int head_size,
                                              bool has_lse,
                                              bool enable_alibi)
{
    return fmha_fwd_traits{head_size,
                           head_size,
                           dtype,
                           true, // is_group_mode
                           true, // is_v_rowmajor
                           mask.type,
                           enable_alibi ? bias_enum::alibi : bias_enum::no_bias,
                           has_lse,
                           false}; // do_fp8_static_quant
}

fmha_fwd_args get_ck_fmha_varlen_fwd_args(bool has_lse,
                                          const mask_info &mask,
                                          // sizes
                                          const int b,
                                          const int max_seqlen_q,
                                          const int h,
                                          const int h_k,
                                          const int d,
                                          // device pointers
                                          const at::Tensor q,
                                          const at::Tensor k,
                                          const at::Tensor v,
                                          const at::Tensor seqlens_q,
                                          const at::Tensor seqlens_k,
                                          c10::optional<at::Tensor> &alibi_slopes_,
                                          at::Tensor out,
                                          at::Tensor softmax_lse,
                                          float softmax_scale)
{
    // q: (total_q, nheads, d)
    // k: (total_k, nheads_k, d)
    // v: (total_k, nheads_k, d)
    // o: (total_q, nheads, d)

    // alibi_slopes:(batch, nheads) or (nhead)
    // lse: (batch, nheads, max_seqlen_q)

    ck_tile::index_t total_q = q.size(0);
    ck_tile::index_t total_k = k.size(0);

    ck_tile::index_t stride_q = q.stride(0);
    ck_tile::index_t stride_k = k.stride(0);
    ck_tile::index_t stride_v = v.stride(0);
    ck_tile::index_t stride_o = out.stride(0);

    ck_tile::index_t nhead_stride_q = q.stride(1);
    ck_tile::index_t nhead_stride_k = k.stride(1);
    ck_tile::index_t nhead_stride_v = v.stride(1);
    ck_tile::index_t nhead_stride_o = out.stride(1);
    ck_tile::index_t nhead_stride_lse = has_lse ? softmax_lse.stride(1) : 0;

    ck_tile::index_t batch_stride_q = 0;
    ck_tile::index_t batch_stride_k = 0;
    ck_tile::index_t batch_stride_v = 0;
    ck_tile::index_t batch_stride_o = 0;

    ck_tile::index_t batch_stride_bias = 0;
    ck_tile::index_t batch_stride_lse = 0;

    void *alibi_slopes_ptr = nullptr;
    ck_tile::index_t stride_alibi_slopes = 0;

    if (alibi_slopes_.has_value())
    {
        auto alibi_slopes = alibi_slopes_.value();
        CHECK_DEVICE(alibi_slopes);
        TORCH_CHECK(alibi_slopes.stride(-1) == 1, "ALiBi slopes tensor must have contiguous last dimension");
        TORCH_CHECK(alibi_slopes.sizes() == torch::IntArrayRef({h}) || alibi_slopes.sizes() == torch::IntArrayRef({b, h}));
        alibi_slopes_ptr = alibi_slopes.data_ptr();
        stride_alibi_slopes = alibi_slopes.dim() == 2 ? alibi_slopes.stride(0) : 0;
    }

    return fmha_fwd_args{q.data_ptr(),
                         k.data_ptr(),
                         v.data_ptr(),
                         alibi_slopes_ptr, // bias
                         has_lse ? softmax_lse.data_ptr() : nullptr,
                         out.data_ptr(),
                         seqlens_q.data_ptr(), // seqstart_q
                         seqlens_k.data_ptr(), // seqstart_k
                         nullptr,
                         total_q,
                         total_k,
                         b,
                         max_seqlen_q,
                         d,             // hdim_q
                         d,             // hdim_v
                         h,             // nhead
                         h_k,           // nhead_k
                         softmax_scale, // scale_s
                         1,             // scale_p
                         1,             // scale_o
                         stride_q,
                         stride_k,
                         stride_v,
                         stride_alibi_slopes,
                         stride_o,
                         nhead_stride_q,
                         nhead_stride_k,
                         nhead_stride_v,
                         0, // nhead_stride_bias, FA without bias
                         nhead_stride_lse,
                         nhead_stride_o,
                         batch_stride_q,
                         batch_stride_k,
                         batch_stride_v,
                         0, // batch_stride_bias, FA without bias
                         batch_stride_lse,
                         batch_stride_o,
                         mask.left,
                         mask.right,
                         static_cast<ck_tile::index_t>(mask.type)};
}

std::vector<at::Tensor>
mha_varlen_fwd(at::Tensor &q,                   // total_q x num_heads x head_size, total_q := \sum_{i=0}^{b} s_i
               const at::Tensor &k,             // total_k x num_heads_k x head_size, total_k := \sum_{i=0}^{b} s_i or num_blocks x page_block_size x num_heads_k x head_size if there's a block_table.
               const at::Tensor &v,             // total_k x num_heads_k x head_size, total_k := \sum_{i=0}^{b} s_i or num_blocks x page_block_size x num_heads_k x head_size if there's a block_table.
               c10::optional<at::Tensor> &out_, // total_q x num_heads x head_size, total_k := \sum_{i=0}^{b} s_i
               const at::Tensor &cu_seqlens_q,  // b+1
               const at::Tensor &cu_seqlens_k,  // b+1
               c10::optional<at::Tensor> & /*seqused_k*/,
               c10::optional<at::Tensor> &block_table_,  // batch_size x max_num_blocks_per_seq
               c10::optional<at::Tensor> &alibi_slopes_, // num_heads or b x num_heads
               int max_seqlen_q,
               const int max_seqlen_k,
               const float p_dropout,
               const float softmax_scale,
               const bool zero_tensors,
               bool is_causal,
               int window_size_left,
               int window_size_right,
               const bool return_softmax,
               c10::optional<at::Generator> /*gen_*/)
{
    auto dprops = at::cuda::getCurrentDeviceProperties();
    bool is_gfx94x = dprops->major == 9 && dprops->minor == 4;
    TORCH_CHECK(is_gfx94x,
                "FlashAttention only supports AMD MI300 GPUs.");

    auto q_dtype = q.dtype();
    TORCH_CHECK(q_dtype == torch::kFloat16 || q_dtype == torch::kBFloat16,
                "FlashAttention only support fp16 and bf16 data type");

    TORCH_CHECK(k.dtype() == q_dtype, "query and key must have the same dtype");
    TORCH_CHECK(v.dtype() == q_dtype, "query and value must have the same dtype");
    TORCH_CHECK(cu_seqlens_q.dtype() == torch::kInt32, "cu_seqlens_q must have dtype int32");
    TORCH_CHECK(cu_seqlens_k.dtype() == torch::kInt32, "cu_seqlens_k must have dtype int32");

    std::string q_dtype_str = q_dtype == torch::kFloat16 ? "fp16" : "bf16";

    CHECK_DEVICE(q);
    CHECK_DEVICE(k);
    CHECK_DEVICE(v);
    CHECK_DEVICE(cu_seqlens_q);
    CHECK_DEVICE(cu_seqlens_k);

    // TODO - Support paged_KV
    const bool paged_KV = block_table_.has_value();
    assert(!paged_KV);

    TORCH_CHECK(q.stride(-1) == 1, "Input tensor must have contiguous last dimension");
    TORCH_CHECK(k.stride(-1) == 1, "Input tensor must have contiguous last dimension");
    TORCH_CHECK(v.stride(-1) == 1, "Input tensor must have contiguous last dimension");
    CHECK_CONTIGUOUS(cu_seqlens_q);
    CHECK_CONTIGUOUS(cu_seqlens_k);

    const auto sizes = q.sizes();

    const int batch_size = cu_seqlens_q.numel() - 1;
    int num_heads = sizes[1];
    const int head_size_og = sizes[2];
    const int num_heads_k = k.size(1);

    const int max_num_blocks_per_seq = 0;
    const int num_blocks = 0;
    const int page_block_size = 1;

    TORCH_CHECK(batch_size > 0, "batch size must be postive");
    TORCH_CHECK(head_size_og <= 256, "CK only supports head dimension at most 256");
    TORCH_CHECK(num_heads % num_heads_k == 0, "Number of heads in key/value must divide number of heads in query");

    if (window_size_left >= max_seqlen_k)
    {
        window_size_left = -1;
    }
    if (window_size_right >= max_seqlen_k)
    {
        window_size_right = -1;
    }

    // causal=true is the same as causal=false in this case
    if (max_seqlen_q == 1 && !alibi_slopes_.has_value())
    {
        is_causal = false;
    }

    mask_info mask;

    if (is_causal)
    {
        // Causal is the special case where window_size_right == 0 and window_size_left < 0.
        window_size_right = 0;
        std::string mask_identify = "b:" + std::to_string(window_size_left) + "," + "0";
        mask = mask_info::decode(mask_identify, max_seqlen_q, max_seqlen_k); // casual
    }
    else if (window_size_left == -1 && window_size_right == -1)
    {
        mask = mask_info::decode("0", max_seqlen_q, max_seqlen_k); // no mask
    }
    else
    {
        // Local is the more general case where window_size_right >= 0 or window_size_left >= 0.
        std::string mask_identify = "b:" + std::to_string(window_size_left) + "," + std::to_string(window_size_right);
        mask = mask_info::decode(mask_identify, max_seqlen_q, max_seqlen_k); // local
    }

    // TODO
    // Faster to transpose q from (b, 1, (nheads_kv ngroups), d) to (b, ngroups, nheads_kv, d) in this case
    // H/t Daniel Haziza

    const int total_q = q.size(0);
    const int total_k = k.size(0);

    CHECK_SHAPE(q, total_q, num_heads, head_size_og);
    CHECK_SHAPE(k, total_k, num_heads_k, head_size_og);
    CHECK_SHAPE(v, total_k, num_heads_k, head_size_og);
    CHECK_SHAPE(cu_seqlens_q, batch_size + 1);
    CHECK_SHAPE(cu_seqlens_k, batch_size + 1);

    at::Tensor q_padded, k_padded, v_padded;
    if (head_size_og % 8 != 0)
    {
        q_padded = torch::nn::functional::pad(q, torch::nn::functional::PadFuncOptions({0, 8 - head_size_og % 8}));
        k_padded = torch::nn::functional::pad(k, torch::nn::functional::PadFuncOptions({0, 8 - head_size_og % 8}));
        v_padded = torch::nn::functional::pad(v, torch::nn::functional::PadFuncOptions({0, 8 - head_size_og % 8}));
    }
    else
    {
        q_padded = q;
        k_padded = k;
        v_padded = v;
    }

    auto round_multiple = [](int x, int m)
    { return (x + m - 1) / m * m; };
    const int head_size_8x = round_multiple(head_size_og, 8);

    at::Tensor out;
    if (out_.has_value())
    {
        out = out_.value();
        TORCH_CHECK(out.dtype() == q_dtype, "Output must have the same dtype as inputs");
        CHECK_DEVICE(out);
        TORCH_CHECK(out.stride(-1) == 1, "Output tensor must have contiguous last dimension");
        CHECK_SHAPE(out, total_q, num_heads, head_size_og);

        if (head_size_og % 8 != 0)
        {
            out = torch::empty_like(q_padded);
        }
    }
    else
    {
        out = torch::empty_like(q_padded);
    }

    // Otherwise the kernel will be launched from cuda:0 device
    // Cast to char to avoid compiler warning about narrowing
    at::cuda::CUDAGuard device_guard{(char)q.get_device()};

    auto opts = q.options();

    // TODO - return P if return_softmax == true
    at::Tensor p;

    at::Tensor softmax_lse;
    if (return_softmax)
        softmax_lse = torch::empty({batch_size, num_heads, max_seqlen_q}, opts.dtype(at::kFloat));
    // TODO - Support dropout
    assert(p_dropout == 0.f);
    auto options = torch::TensorOptions().dtype(torch::kFloat32).device(torch::kCUDA);
    auto rng_state = torch::empty({2}, options.dtype(torch::kInt64));

    if (zero_tensors)
    {
        out.zero_();
        if (return_softmax)
        {
            softmax_lse.fill_(-std::numeric_limits<float>::infinity());
            p.zero_();
        }
    }

    if (max_seqlen_k > 0)
    {
        auto stream = at::cuda::getCurrentHIPStream().stream();
        ck_tile::stream_config stream_config{stream, false, 0, 0, 0};

        auto traits =
            get_ck_fmha_varlen_fwd_traits(mask, q_dtype_str, head_size_8x, return_softmax, alibi_slopes_.has_value());

        auto args =
            get_ck_fmha_varlen_fwd_args(
                return_softmax,
                mask,
                batch_size,
                max_seqlen_q,
                num_heads,
                num_heads_k,
                head_size_8x,
                q_padded,
                k_padded,
                v_padded,
                cu_seqlens_q,
                cu_seqlens_k,
                alibi_slopes_,
                out,
                softmax_lse,
                softmax_scale);

        fmha_fwd(traits, args, stream_config);
    }
    else
    {
        // If seqlen_k == 0, then we have an empty tensor. We need to set the output to 0.
        out.zero_();
        if (return_softmax)
            softmax_lse.fill_(std::numeric_limits<float>::infinity());
    }

    at::Tensor out_padded = out;
    if (head_size_og % 8 != 0)
    {
        out = out.index({"...", torch::indexing::Slice(torch::indexing::None, head_size_og)});
        if (out_.has_value())
        {
            out_.value().copy_(out);
        }
    }

    return {out, q_padded, k_padded, v_padded, out_padded, softmax_lse, p, rng_state};
}
