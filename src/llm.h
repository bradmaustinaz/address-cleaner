#ifndef LLM_H
#define LLM_H

#include <stddef.h>

/*
 * llm.h — Optional AI fallback via llama-server sidecar.
 *
 * If "llama-server.exe" and a "*.gguf" model file are found next to this
 * executable, the server is started automatically in a background thread
 * and used as a fallback for names the rule engine could not fully clean.
 *
 * The app degrades gracefully — if no model is found, llm_is_ready()
 * returns 0 and the rules-cleaned result is kept as-is.
 *
 * Recommended model: qwen2.5-1.5b-instruct-q4_k_m.gguf  (~1 GB)
 * Download llama-server.exe (CUDA build) from the llama.cpp releases page.
 */

/*
 * Start the sidecar in a background thread. Non-blocking — the server
 * may still be loading its model when this returns. Check llm_is_ready().
 */
void llm_init(void);

/*
 * Shut down the sidecar process. Safe to call even if llm_init was not.
 */
void llm_shutdown(void);

/*
 * Returns 1 once the server is loaded and accepting requests, 0 otherwise.
 */
int llm_is_ready(void);

/*
 * Ask the LLM to clean one name.
 *   raw:          original un-cleaned input (for context)
 *   rules_result: what the rule engine already produced
 *   out / outlen: buffer for the AI's answer
 *
 * Returns 1 on success (out filled), 0 on any failure (keep rules_result).
 * The result has leading/trailing whitespace stripped.
 */
int llm_clean_name(const char *raw, const char *rules_result,
                   char *out, size_t outlen);

/*
 * Human-readable AI status string for the status bar.
 * Returns one of: "AI: Ready", "AI: Loading...", "AI: No model"
 */
const char *llm_status_str(void);

#endif /* LLM_H */
