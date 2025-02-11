#define LLAMAMODEL_H_I_KNOW_WHAT_I_AM_DOING_WHEN_INCLUDING_THIS_FILE
#include "llamamodel_impl.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <llama.h>
#include <ggml.h>
#ifdef GGML_USE_KOMPUTE
#include <ggml-kompute.h>
#endif

using namespace std::string_literals;

// Maximum supported GGUF version
static constexpr int GGUF_VER_MAX = 3;

static const char * const modelType_ = "LLaMA";

static const std::vector<const char *> KNOWN_ARCHES {
    "baichuan", "bert", "bloom", "codeshell", "falcon", "gemma", "gpt2", "llama", "mpt", "nomic-bert", "orion",
    "persimmon", "phi2", "plamo", "qwen", "qwen2", "refact", "stablelm", "starcoder"
};

static const std::vector<const char *> EMBEDDING_ARCHES {
    "bert", "nomic-bert"
};

static bool is_embedding_arch(const std::string &arch) {
    return std::find(EMBEDDING_ARCHES.begin(), EMBEDDING_ARCHES.end(), arch) < EMBEDDING_ARCHES.end();
}

static bool llama_verbose() {
    const char* var = getenv("GPT4ALL_VERBOSE_LLAMACPP");
    return var && *var;
}

static void llama_log_callback(enum ggml_log_level level, const char *text, void *userdata) {
    (void)userdata;
    if (llama_verbose() || level <= GGML_LOG_LEVEL_ERROR) {
        fputs(text, stderr);
    }
}

struct gpt_params {
    int32_t seed          = -1;   // RNG seed
    int32_t n_keep        = 0;    // number of tokens to keep from initial prompt

    // sampling parameters
    float   tfs_z         = 1.0f; // 1.0 = disabled
    float   typical_p     = 1.0f; // 1.0 = disabled

    std::string prompt = "";

    enum ggml_type kv_type = GGML_TYPE_F16; // use f16 instead of f32 for memory kv

    bool use_mmap          = true;  // use mmap for faster loads
    bool use_mlock         = false; // use mlock to keep model in memory
};

static int llama_sample_top_p_top_k(
        llama_context *ctx,
        const llama_token *last_n_tokens_data,
        int last_n_tokens_size,
        int top_k,
        float top_p,
        float min_p,
        float temp,
        float repeat_penalty,
        int32_t pos) {
    auto logits = llama_get_logits_ith(ctx, pos);
    auto n_vocab = llama_n_vocab(llama_get_model(ctx));
    // Populate initial list of all candidates
    std::vector<llama_token_data> candidates;
    candidates.reserve(n_vocab);
    for (int token_id = 0; token_id < n_vocab; token_id++) {
        candidates.emplace_back(llama_token_data{token_id, logits[token_id], 0.0f});
    }
    llama_token_data_array candidates_p = {candidates.data(), candidates.size(), false};
    // Sample repeat penalty
    llama_sample_repetition_penalties(nullptr, &candidates_p, last_n_tokens_data, last_n_tokens_size, repeat_penalty, 0.0f, 0.0f);
    // Temperature sampling
    llama_sample_top_k(ctx, &candidates_p, top_k, 1);
    llama_sample_tail_free(ctx, &candidates_p, 1.0f, 1);
    llama_sample_typical(ctx, &candidates_p, 1.0f, 1);
    llama_sample_top_p(ctx, &candidates_p, top_p, 1);
    llama_sample_min_p(ctx, &candidates_p, min_p, 1);
    llama_sample_temp(ctx, &candidates_p, temp);
    return llama_sample_token(ctx, &candidates_p);
}

std::string get_arch_name(gguf_context *ctx_gguf) {
    std::string arch_name;
    const int kid = gguf_find_key(ctx_gguf, "general.architecture");
    enum gguf_type ktype = gguf_get_kv_type(ctx_gguf, kid);
    if (ktype != (GGUF_TYPE_STRING)) {
        throw std::runtime_error("ERROR: Can't get general architecture from gguf file.");
    }
    return gguf_get_val_str(ctx_gguf, kid);
}

static gguf_context *load_gguf(const char *fname) {
    struct gguf_init_params params = {
        /*.no_alloc = */ true,
        /*.ctx      = */ nullptr,
    };
    gguf_context *ctx = gguf_init_from_file(fname, params);
    if (!ctx) {
        std::cerr << __func__ << ": gguf_init_from_file failed\n";
        return nullptr;
    }

    int gguf_ver = gguf_get_version(ctx);
    if (gguf_ver > GGUF_VER_MAX) {
        std::cerr << __func__ << ": unsupported gguf version: " << gguf_ver << "\n";
        gguf_free(ctx);
        return nullptr;
    }

    return ctx;
}

static int32_t get_arch_key_u32(std::string const &modelPath, std::string const &archKey) {
    auto * ctx = load_gguf(modelPath.c_str());
    if (!ctx)
        return -1;
    std::string arch = get_arch_name(ctx);

    int32_t value = -1;
    if (ctx) {
        auto key = arch + "." + archKey;
        int keyidx = gguf_find_key(ctx, key.c_str());
        if (keyidx != -1) {
            value = gguf_get_val_u32(ctx, keyidx);
        } else {
            std::cerr << __func__ << ": " << key << "not found in " << modelPath << "\n";
        }
    }

    gguf_free(ctx);
    return value;
}

struct LLamaPrivate {
    const std::string modelPath;
    bool modelLoaded;
    int device = -1;
    llama_model *model = nullptr;
    llama_context *ctx = nullptr;
    llama_model_params model_params;
    llama_context_params ctx_params;
    int64_t n_threads = 0;
    std::vector<LLModel::Token> end_tokens;
};

LLamaModel::LLamaModel()
    : d_ptr(new LLamaPrivate) {
    d_ptr->modelLoaded = false;
}

// default hparams (LLaMA 7B)
struct llama_file_hparams {
    uint32_t n_vocab = 32000;
    uint32_t n_embd  = 4096;
    uint32_t n_mult  = 256;
    uint32_t n_head  = 32;
    uint32_t n_layer = 32;
    uint32_t n_rot   = 64;
    enum llama_ftype ftype = LLAMA_FTYPE_MOSTLY_F16;
};

size_t LLamaModel::requiredMem(const std::string &modelPath, int n_ctx, int ngl) {
    // TODO(cebtenzzre): update to GGUF
    (void)ngl; // FIXME(cetenzzre): use this value
    auto fin = std::ifstream(modelPath, std::ios::binary);
    fin.seekg(0, std::ios_base::end);
    size_t filesize = fin.tellg();
    fin.seekg(0, std::ios_base::beg);
    uint32_t magic = 0;
    fin.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (magic != 0x67676a74) return 0;
    uint32_t version = 0;
    fin.read(reinterpret_cast<char*>(&version), sizeof(version));
    llama_file_hparams hparams;
    fin.read(reinterpret_cast<char*>(&hparams.n_vocab), sizeof(hparams.n_vocab));
    fin.read(reinterpret_cast<char*>(&hparams.n_embd), sizeof(hparams.n_embd));
    fin.read(reinterpret_cast<char*>(&hparams.n_head), sizeof(hparams.n_head));
    fin.read(reinterpret_cast<char*>(&hparams.n_layer), sizeof(hparams.n_layer));
    fin.read(reinterpret_cast<char*>(&hparams.n_rot), sizeof(hparams.n_rot));
    fin.read(reinterpret_cast<char*>(&hparams.ftype), sizeof(hparams.ftype));
    const size_t kvcache_element_size = 2; // fp16
    const size_t est_kvcache_size = hparams.n_embd * hparams.n_layer * 2u * n_ctx * kvcache_element_size;
    return filesize + est_kvcache_size;
}

bool LLamaModel::isModelBlacklisted(const std::string &modelPath) const {
    auto * ctx = load_gguf(modelPath.c_str());
    if (!ctx) {
        std::cerr << __func__ << ": failed to load " << modelPath << "\n";
        return false;
    }

    auto get_key = [ctx, &modelPath](const char *name) {
        int keyidx = gguf_find_key(ctx, name);
        if (keyidx == -1) {
            throw std::logic_error(name + " not found in "s + modelPath);
        }
        return keyidx;
    };

    bool res = false;
    try {
        std::string name(gguf_get_val_str(ctx, get_key("general.name")));
        int token_idx = get_key("tokenizer.ggml.tokens");
        int n_vocab = gguf_get_arr_n(ctx, token_idx);

        // check for known bad models
        if (name == "open-orca_mistral-7b-openorca"
            && n_vocab == 32002
            && gguf_get_arr_str(ctx, token_idx, 32000) == "<dummy32000>"s // should be <|im_end|>
        ) {
            res = true;
        }
    } catch (const std::logic_error &e) {
        std::cerr << __func__ << ": " << e.what() << "\n";
    }

    gguf_free(ctx);
    return res;
}

bool LLamaModel::isEmbeddingModel(const std::string &modelPath) const {
    auto *ctx_gguf = load_gguf(modelPath.c_str());
    if (!ctx_gguf) {
        std::cerr << __func__ << ": failed to load GGUF from " <<  modelPath << "\n";
        return false;
    }

    std::string arch = get_arch_name(ctx_gguf);
    gguf_free(ctx_gguf);
    return is_embedding_arch(arch);
}

bool LLamaModel::loadModel(const std::string &modelPath, int n_ctx, int ngl)
{
    d_ptr->modelLoaded = false;

    // clean up after previous loadModel()
    if (d_ptr->model) {
        llama_free_model(d_ptr->model);
        d_ptr->model = nullptr;
    }
    if (d_ptr->ctx) {
        llama_free(d_ptr->ctx);
        d_ptr->ctx = nullptr;
    }

    if (n_ctx < 8) {
        std::cerr << "warning: minimum context size is 8, using minimum size.\n";
        n_ctx = 8;
    }

    // -- load the model --

    gpt_params params;

    d_ptr->model_params = llama_model_default_params();

    d_ptr->model_params.use_mmap  = params.use_mmap;
#if defined (__APPLE__)
    d_ptr->model_params.use_mlock = true;
#else
    d_ptr->model_params.use_mlock = params.use_mlock;
#endif

    d_ptr->model_params.progress_callback = &LLModel::staticProgressCallback;
    d_ptr->model_params.progress_callback_user_data = this;

#ifdef GGML_USE_KOMPUTE
    if (d_ptr->device != -1) {
        d_ptr->model_params.main_gpu = d_ptr->device;
        d_ptr->model_params.n_gpu_layers = ngl;
    }
#elif defined(GGML_USE_METAL)
    (void)ngl;

    if (llama_verbose()) {
        std::cerr << "llama.cpp: using Metal" << std::endl;
    }

    // always fully offload on Metal
    // TODO(cebtenzzre): use this parameter to allow using more than 53% of system RAM to load a model
    d_ptr->model_params.n_gpu_layers = 100;
#else
    (void)ngl;
#endif

    d_ptr->model = llama_load_model_from_file_gpt4all(modelPath.c_str(), &d_ptr->model_params);
    if (!d_ptr->model) {
        fflush(stdout);
        d_ptr->device = -1;
        std::cerr << "LLAMA ERROR: failed to load model from " << modelPath << std::endl;
        return false;
    }

    // -- initialize the context --

    d_ptr->ctx_params = llama_context_default_params();

    bool isEmbedding = is_embedding_arch(llama_model_arch(d_ptr->model));
    const int n_ctx_train = llama_n_ctx_train(d_ptr->model);
    if (isEmbedding) {
        d_ptr->ctx_params.n_batch = n_ctx_train;
    } else {
        if (n_ctx > n_ctx_train) {
            std::cerr << "warning: model was trained on only " << n_ctx_train << " context tokens ("
                      << n_ctx << " specified)\n";
        }
    }

    d_ptr->ctx_params.n_ctx   = n_ctx;
    d_ptr->ctx_params.seed    = params.seed;
    d_ptr->ctx_params.type_k  = params.kv_type;
    d_ptr->ctx_params.type_v  = params.kv_type;

    // The new batch API provides space for n_vocab*n_tokens logits. Tell llama.cpp early
    // that we want this many logits so the state serializes consistently.
    d_ptr->ctx_params.logits_all = true;

    d_ptr->n_threads = std::min(4, (int32_t) std::thread::hardware_concurrency());
    d_ptr->ctx_params.n_threads       = d_ptr->n_threads;
    d_ptr->ctx_params.n_threads_batch = d_ptr->n_threads;

    if (isEmbedding)
        d_ptr->ctx_params.embeddings = true;

    d_ptr->ctx = llama_new_context_with_model(d_ptr->model, d_ptr->ctx_params);
    if (!d_ptr->ctx) {
        fflush(stdout);
        std::cerr << "LLAMA ERROR: failed to init context for model " <<  modelPath << std::endl;
        llama_free_model(d_ptr->model);
        d_ptr->model = nullptr;
        d_ptr->device = -1;
        return false;
    }

    d_ptr->end_tokens = {llama_token_eos(d_ptr->model)};

#ifdef GGML_USE_KOMPUTE
    if (usingGPUDevice() && ggml_vk_has_device()) {
        std::cerr << "llama.cpp: using Vulkan on " << ggml_vk_current_device().name << std::endl;
    }
#endif

    m_supportsEmbedding = isEmbedding;
    m_supportsCompletion = !isEmbedding;

    fflush(stdout);
    d_ptr->modelLoaded = true;
    return true;
}

void LLamaModel::setThreadCount(int32_t n_threads) {
    d_ptr->n_threads = n_threads;
    llama_set_n_threads(d_ptr->ctx, n_threads, n_threads);
}

int32_t LLamaModel::threadCount() const {
    return d_ptr->n_threads;
}

LLamaModel::~LLamaModel()
{
    if (d_ptr->ctx) {
        llama_free(d_ptr->ctx);
    }
    llama_free_model(d_ptr->model);
}

bool LLamaModel::isModelLoaded() const
{
    return d_ptr->modelLoaded;
}

size_t LLamaModel::stateSize() const
{
    return llama_get_state_size(d_ptr->ctx);
}

size_t LLamaModel::saveState(uint8_t *dest) const
{
    return llama_copy_state_data(d_ptr->ctx, dest);
}

size_t LLamaModel::restoreState(const uint8_t *src)
{
    // const_cast is required, see: https://github.com/ggerganov/llama.cpp/pull/1540
    return llama_set_state_data(d_ptr->ctx, const_cast<uint8_t*>(src));
}

std::vector<LLModel::Token> LLamaModel::tokenize(PromptContext &ctx, const std::string &str, bool special) const
{
    const bool wantBOS = ctx.n_past == 0 && ctx.tokens.empty();
    const bool useBOS = wantBOS && shouldAddBOS();
    auto strCat = wantBOS && !special ? " " + str : str; // insert leading space ourselves, llama.cpp fork doesn't anymore
    std::vector<LLModel::Token> fres(strCat.size()+4);
    auto fres_len = llama_tokenize(d_ptr->model, strCat.c_str(), strCat.length(), fres.data(), fres.size(), useBOS, special);
    fres.resize(fres_len);
    return fres;
}

std::string LLamaModel::tokenToString(Token id) const
{
    return llama_token_to_piece(d_ptr->ctx, id);
}

LLModel::Token LLamaModel::sampleToken(PromptContext &promptCtx) const
{
    const size_t n_prev_toks = std::min((size_t) promptCtx.repeat_last_n, promptCtx.tokens.size());
    return llama_sample_top_p_top_k(d_ptr->ctx,
        promptCtx.tokens.data() + promptCtx.tokens.size() - n_prev_toks,
        n_prev_toks, promptCtx.top_k, promptCtx.top_p, promptCtx.min_p, promptCtx.temp,
        promptCtx.repeat_penalty, promptCtx.n_last_batch_tokens - 1);
}

bool LLamaModel::evalTokens(PromptContext &ctx, const std::vector<int32_t> &tokens) const
{
    llama_kv_cache_seq_rm(d_ptr->ctx, 0, ctx.n_past, -1);

    llama_batch batch = llama_batch_init(tokens.size(), 0, 1);

    batch.n_tokens = tokens.size();
    ctx.n_last_batch_tokens = tokens.size();

    for (int32_t i = 0; i < batch.n_tokens; i++) {
        batch.token   [i] = tokens[i];
        batch.pos     [i] = ctx.n_past + i;
        batch.n_seq_id[i] = 1;
        batch.seq_id  [i][0] = 0;
        batch.logits  [i] = false;
    }

    // llama_decode will output logits only for the last token of the prompt
    batch.logits[batch.n_tokens - 1] = true;

    int res = llama_decode(d_ptr->ctx, batch);
    llama_batch_free(batch);
    return res == 0;
}

int32_t LLamaModel::contextLength() const
{
    return llama_n_ctx(d_ptr->ctx);
}

const std::vector<LLModel::Token> &LLamaModel::endTokens() const
{
    return d_ptr->end_tokens;
}

bool LLamaModel::shouldAddBOS() const
{
    int add_bos = llama_add_bos_token(d_ptr->model);
    return add_bos != -1 ? bool(add_bos) : llama_vocab_type(d_ptr->model) == LLAMA_VOCAB_TYPE_SPM;
}

int32_t LLamaModel::maxContextLength(std::string const &modelPath) const
{
    return get_arch_key_u32(modelPath, "context_length");
}

int32_t LLamaModel::layerCount(std::string const &modelPath) const
{
    return get_arch_key_u32(modelPath, "block_count");
}

std::vector<LLModel::GPUDevice> LLamaModel::availableGPUDevices(size_t memoryRequired) const
{
#ifdef GGML_USE_KOMPUTE
    size_t count = 0;
    auto * vkDevices = ggml_vk_available_devices(memoryRequired, &count);

    if (vkDevices) {
        std::vector<LLModel::GPUDevice> devices;
        devices.reserve(count);

        for (size_t i = 0; i < count; ++i) {
            auto & dev = vkDevices[i];
            devices.emplace_back(
                /* index    = */ dev.index,
                /* type     = */ dev.type,
                /* heapSize = */ dev.heapSize,
                /* name     = */ dev.name,
                /* vendor   = */ dev.vendor
            );
            ggml_vk_device_destroy(&dev);
        }

        free(vkDevices);
        return devices;
    }
#else
    (void)memoryRequired;
    std::cerr << __func__ << ": built without Kompute\n";
#endif

    return {};
}

bool LLamaModel::initializeGPUDevice(size_t memoryRequired, const std::string &name) const
{
#if defined(GGML_USE_KOMPUTE)
    ggml_vk_device device;
    bool ok = ggml_vk_get_device(&device, memoryRequired, name.c_str());
    if (ok) {
        d_ptr->device = device.index;
        return true;
    }
#else
    (void)memoryRequired;
    (void)name;
#endif
    return false;
}

bool LLamaModel::initializeGPUDevice(int device, std::string *unavail_reason) const
{
#if defined(GGML_USE_KOMPUTE)
    (void)unavail_reason;
    d_ptr->device = device;
    return true;
#else
    (void)device;
    if (unavail_reason) {
        *unavail_reason = "built without Kompute";
    }
    return false;
#endif
}

bool LLamaModel::hasGPUDevice()
{
#if defined(GGML_USE_KOMPUTE)
    return d_ptr->device != -1;
#else
    return false;
#endif
}

bool LLamaModel::usingGPUDevice()
{
#if defined(GGML_USE_KOMPUTE)
    return hasGPUDevice() && d_ptr->model_params.n_gpu_layers > 0;
#elif defined(GGML_USE_METAL)
    return true;
#else
    return false;
#endif
}

void llama_batch_add(
                 struct llama_batch & batch,
                        llama_token   id,
                          llama_pos   pos,
    const std::vector<llama_seq_id> & seq_ids,
                               bool   logits) {
    batch.token   [batch.n_tokens] = id;
    batch.pos     [batch.n_tokens] = pos;
    batch.n_seq_id[batch.n_tokens] = seq_ids.size();
    for (size_t i = 0; i < seq_ids.size(); ++i) {
        batch.seq_id[batch.n_tokens][i] = seq_ids[i];
    }
    batch.logits  [batch.n_tokens] = logits;

    batch.n_tokens++;
}

static void batch_add_seq(llama_batch &batch, const std::vector<LLModel::Token> &tokens, int seq_id) {
    for (unsigned i = 0; i < tokens.size(); i++) {
        llama_batch_add(batch, tokens[i], i, { seq_id }, i == tokens.size() - 1);
    }
}

size_t LLamaModel::embeddingSize() const {
    return llama_n_embd(d_ptr->model);
}

struct EmbModelSpec {
    const char *docPrefix;
    const char *queryPrefix;
    std::vector<const char *> otherPrefixes = {};
    bool matryoshkaCapable = false;
    const char *recommendedDims = nullptr;
};

struct EmbModelGroup {
    EmbModelSpec spec;
    std::vector<const char *> names;
};

static const EmbModelSpec NOPREFIX_SPEC {"", ""};
static const EmbModelSpec NOMIC_SPEC    {"search_document", "search_query", {"clustering", "classification"}};
static const EmbModelSpec E5_SPEC       {"passage", "query"};

static const EmbModelSpec NOMIC_1_5_SPEC {
    "search_document", "search_query", {"clustering", "classification"}, true, "[768, 512, 384, 256, 128]",
};
static const EmbModelSpec LLM_EMBEDDER_SPEC {
    "Represent this document for retrieval",
    "Represent this query for retrieving relevant documents",
};
static const EmbModelSpec BGE_SPEC {
    "", "Represent this sentence for searching relevant passages",
};
static const EmbModelSpec E5_MISTRAL_SPEC {
    "", "Instruct: Given a query, retrieve relevant passages that answer the query\nQuery",
};

static const EmbModelGroup EMBEDDING_MODEL_SPECS[] {
    {NOPREFIX_SPEC,     {"all-MiniLM-L6-v1", "all-MiniLM-L12-v1", "all-MiniLM-L6-v2", "all-MiniLM-L12-v2"}},
    {NOMIC_SPEC,        {"nomic-embed-text-v1", "nomic-embed-text-v1-ablated", "nomic-embed-text-v1-unsupervised"}},
    {NOMIC_1_5_SPEC,    {"nomic-embed-text-v1.5"}},
    {LLM_EMBEDDER_SPEC, {"llm-embedder"}},
    {BGE_SPEC,          {"bge-small-en", "bge-base-en", "bge-large-en",
                         "bge-small-en-v1.5", "bge-base-en-v1.5", "bge-large-en-v1.5"}},
    {E5_SPEC,           {"e5-small", "e5-base", "e5-large",
                         "e5-small-unsupervised", "e5-base-unsupervised", "e5-large-unsupervised",
                         "e5-small-v2", "e5-base-v2", "e5-large-v2"}},
    {E5_MISTRAL_SPEC,   {"e5-mistral-7b-instruct",
                         "multilingual-e5-small", "multilingual-e5-base", "multilingual-e5-large",
                         "multilingual-e5-large-instruct"}},
};

static const EmbModelSpec *getEmbedSpec(const std::string &modelName) {
    static const auto &specs = EMBEDDING_MODEL_SPECS;
    auto it = std::find_if(specs, std::end(specs),
        [&modelName](auto &spec) {
            auto &names = spec.names;
            return std::find(names.begin(), names.end(), modelName) < names.end();
        }
    );
    return it < std::end(specs) ? &it->spec : nullptr;
}

void LLamaModel::embed(
    const std::vector<std::string> &texts, float *embeddings, bool isRetrieval, int dimensionality, bool doMean,
    bool atlas
) {
    const EmbModelSpec *spec;
    std::optional<std::string> prefix;
    if (d_ptr->model && (spec = getEmbedSpec(llama_model_name(d_ptr->model))))
        prefix = isRetrieval ? spec->queryPrefix : spec->docPrefix;

    embed(texts, embeddings, prefix, dimensionality, doMean, atlas);
}

void LLamaModel::embed(
    const std::vector<std::string> &texts, float *embeddings, std::optional<std::string> prefix, int dimensionality,
    bool doMean, bool atlas
) {
    if (!d_ptr->model)
        throw std::logic_error("no model is loaded");

    const char *modelName = llama_model_name(d_ptr->model);
    if (!m_supportsEmbedding)
        throw std::logic_error("not an embedding model: "s + modelName);

    auto *spec = getEmbedSpec(modelName);
    if (!spec)
        std::cerr << __func__ << ": warning: unknown model " << modelName << "\n";

    const int32_t n_embd = llama_n_embd(d_ptr->model);
    if (dimensionality < 0) {
        dimensionality = n_embd;
    } else if (spec && dimensionality != n_embd) {
        auto msg = [dimensionality, modelName]() {
            return "unsupported dimensionality " + std::to_string(dimensionality) + " for model " + modelName;
        };
        if (!spec->matryoshkaCapable)
            throw std::logic_error(msg() + " (supported: " + std::to_string(n_embd) + ")");
        if (dimensionality == 0 || dimensionality > n_embd)
            throw std::logic_error(msg() + " (recommended: " + spec->recommendedDims + ")");
    }

    if (!prefix) {
        if (spec) {
            prefix = spec->docPrefix;
        } else {
            std::cerr << __func__ << ": warning: assuming no prefix\n";
            prefix = "";
        }
    } else if (spec && prefix != spec->docPrefix && prefix != spec->queryPrefix &&
               std::find(spec->otherPrefixes.begin(), spec->otherPrefixes.end(), *prefix) == spec->otherPrefixes.end())
    {
        std::stringstream ss;
        ss << std::quoted(*prefix) << " is not a valid task type for model " << modelName;
        throw std::logic_error(ss.str());
    }

    embedInternal(texts, embeddings, *prefix, dimensionality, doMean, atlas, spec);
}

// MD5 hash of "nomic empty"
static const char EMPTY_PLACEHOLDER[] = "24df574ea1c998de59d5be15e769658e";

auto product(double a) -> std::function<double(double)> {
    return [a](double b) { return a * b; };
}

template <typename T>
double getL2NormScale(T *start, T *end) {
    double magnitude = std::sqrt(std::inner_product(start, end, start, 0.0));
    return 1.0 / std::max(magnitude, 1e-12);
}

void LLamaModel::embedInternal(
    const std::vector<std::string> &texts, float *embeddings, std::string prefix, int dimensionality,
    bool doMean, bool atlas, const EmbModelSpec *spec
) {
    typedef std::vector<LLModel::Token> TokenString;
    static constexpr int32_t atlasMaxLength = 8192;
    static constexpr int chunkOverlap = 8; // Atlas overlaps n_batch-sized chunks of input by 8 tokens

    const llama_token bos_token = llama_token_bos(d_ptr->model);
    const llama_token eos_token = llama_token_eos(d_ptr->model);

    bool useBOS = shouldAddBOS();
    bool useEOS = llama_vocab_type(d_ptr->model) == LLAMA_VOCAB_TYPE_WPM;

    // no EOS, optional BOS
    auto tokenize = [this, useBOS, useEOS, eos_token](std::string text, TokenString &tokens, bool wantBOS) {
        if (!text.empty() && text[0] != ' ') {
            text = ' ' + text; // normalize for SPM - our fork of llama.cpp doesn't add a space prefix
        }
        wantBOS &= useBOS;

        tokens.resize(text.length()+4);
        int32_t n_tokens = llama_tokenize(d_ptr->model, text.c_str(), text.length(), tokens.data(), tokens.size(), wantBOS, false);
        assert(useEOS == (eos_token != -1 && tokens[n_tokens - 1] == eos_token));
        tokens.resize(n_tokens - useEOS); // erase EOS/SEP
    };

    // tokenize the texts
    std::vector<TokenString> inputs;
    for (unsigned i = 0; i < texts.size(); i++) {
        auto &text = texts[i];
        auto &inp = inputs.emplace_back();
        tokenize(text, inp, false);
        if (atlas && inp.size() > atlasMaxLength) {
            if (doMean) {
                throw std::logic_error(
                    "length of text at index " + std::to_string(i) + " is " + std::to_string(inp.size()) +
                    " tokens which exceeds limit of " + std::to_string(atlasMaxLength)
                );
            }
            inp.resize(atlasMaxLength);
        } else if (inp.empty()) {
            if (!atlas || !text.empty()) {
                std::cerr << __func__ << ": warning: chunking tokenized text at index " << std::to_string(i)
                          << " into zero tokens\n";
            }
            tokenize(EMPTY_PLACEHOLDER, inp, false);
        }
    }

    // tokenize the prefix
    TokenString prefixTokens;
    if (prefix.empty()) {
        prefixTokens.push_back(bos_token);
    } else {
        tokenize(prefix + ':', prefixTokens, true);
    }

    const uint32_t n_batch = llama_n_batch(d_ptr->ctx);
    const uint32_t max_len = n_batch - (prefixTokens.size() + useEOS); // minus BOS/CLS and EOS/SEP
    if (chunkOverlap >= max_len) {
        throw std::logic_error("max chunk length of " + std::to_string(max_len) + " is smaller than overlap of " +
                               std::to_string(chunkOverlap) + " tokens");
    }

    // split into max_len-sized chunks
    struct split_batch { unsigned idx; TokenString batch; };
    std::vector<split_batch> batches;
    for (unsigned i = 0; i < inputs.size(); i++) {
        auto &input = inputs[i];
        for (auto it = input.begin(); it < input.end(); it += max_len) {
            if (it > input.begin()) { it -= chunkOverlap; }
            auto end = std::min(it + max_len, input.end());
            batches.push_back({ i, {} });
            auto &batch = batches.back().batch;
            batch = prefixTokens;
            batch.insert(batch.end(), it, end);
            batch.push_back(eos_token);
            if (!doMean) { break; /* limit text to one chunk */ }
        }
    }
    inputs.clear();

    // initialize batch
    struct llama_batch batch = llama_batch_init(n_batch, 0, 1);

    // n_texts x n_embd matrix
    const int32_t n_embd = llama_n_embd(d_ptr->model);
    std::vector<double> embeddingsSum(texts.size() * n_embd);
    std::vector<int> embeddingsSumTotal(texts.size());
    std::vector<int> queued_indices; // text indices of batches to be processed

    auto decode = [this, &queued_indices, n_embd, &batch, &embeddingsSum, &embeddingsSumTotal, spec, dimensionality]() {
        if (llama_decode(d_ptr->ctx, batch) < 0)
            throw std::runtime_error("llama_decode failed");

        for (int i = 0; i < batch.n_tokens; ++i) {
            if (!batch.logits[i]) { continue; }
            int i_prompt = queued_indices[batch.seq_id[i][0]];
            auto *out = &embeddingsSum[i_prompt * n_embd];

            // sequence embeddings aren't available when pooling_type is NONE
            auto *embd = llama_get_embeddings_seq(d_ptr->ctx, batch.seq_id[i][0]);
            if (!embd) { embd = llama_get_embeddings_ith(d_ptr->ctx, i); }
            assert(embd);

            auto *embd_end = embd + n_embd;

            // layer normalization for nomic-embed-text-v1.5
            if (spec && spec->matryoshkaCapable) {
                // normalize mean
                double mean = std::accumulate(embd, embd_end, 0.0) / n_embd;
                std::transform(embd, embd_end, embd, [mean](double f){ return f - mean; });

                // unbiased sample variance, with Bessel's correction
                double variance = std::inner_product(embd, embd_end, embd, 0.0) / (n_embd - 1);

                // trim to matryoshka dim
                embd_end = embd + dimensionality;

                // normalize variance
                std::transform(embd, embd_end, embd, product(1.0 / std::sqrt(variance + 1e-5)));
            }

            // L2 norm
            auto scale = getL2NormScale(embd, embd_end);
            std::transform(embd, embd_end, out, out, [scale](double e, double o){ return o + scale * e; });
            embeddingsSumTotal[i_prompt]++;
        }
    };

    // break into batches
    for (auto &inp: batches) {
        // encode if at capacity
        if (batch.n_tokens + inp.batch.size() > n_batch) {
            decode();
            batch.n_tokens = 0;
            queued_indices.clear();
        }

        // add to batch
        batch_add_seq(batch, inp.batch, queued_indices.size());
        queued_indices.push_back(inp.idx);
    }

    // final batch
    decode();

    for (unsigned i = 0; i < texts.size(); i++) {
        auto *embd = &embeddingsSum[i * n_embd];
        auto *embd_end = embd + dimensionality;
        int total = embeddingsSumTotal[i];

        // average over chunks
        std::transform(embd, embd_end, embd, product(1.0 / total));

        // L2 norm and copy
        auto scale = getL2NormScale(embd, embd_end);
        std::transform(embd, embd_end, embeddings, product(scale));
        embeddings += dimensionality;
    }
}

#if defined(_WIN32)
#define DLL_EXPORT __declspec(dllexport)
#else
#define DLL_EXPORT __attribute__ ((visibility ("default")))
#endif

extern "C" {
DLL_EXPORT bool is_g4a_backend_model_implementation() {
    return true;
}

DLL_EXPORT const char *get_model_type() {
    return modelType_;
}

DLL_EXPORT const char *get_build_variant() {
    return GGML_BUILD_VARIANT;
}

DLL_EXPORT bool magic_match(const char *fname) {
    auto * ctx = load_gguf(fname);
    std::string arch = get_arch_name(ctx);

    bool valid = true;

    if (std::find(KNOWN_ARCHES.begin(), KNOWN_ARCHES.end(), arch) == KNOWN_ARCHES.end()) {
        // not supported by this version of llama.cpp
        if (arch != "gptj") { // we support this via another module
            std::cerr << __func__ << ": unsupported model architecture: " << arch << "\n";
        }
        valid = false;
    }

    if (valid && is_embedding_arch(arch) && gguf_find_key(ctx, (arch + ".pooling_type").c_str()) < 0)
        valid = false; // old pre-llama.cpp embedding model, e.g. all-MiniLM-L6-v2-f16.gguf

    gguf_free(ctx);
    return valid;
}

DLL_EXPORT LLModel *construct() {
    llama_log_set(llama_log_callback, nullptr);
    return new LLamaModel;
}
}
