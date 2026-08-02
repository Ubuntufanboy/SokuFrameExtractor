// =========================================================================
// SokuFrameExtractor — ogl_hook.cpp
// =========================================================================
//
// Changes from original
// ---------------------
//   Bug 4a — fn_glGetError and fn_glBindFramebuffer were loaded but absent
//             from the null-check in loadGLFunctions().  If either failed
//             to resolve, m_gl_loaded was left false while the caller
//             already set m_gl_loaded = true to suppress log spam, leaving
//             a silent null-function pointer that crashed on first call.
//             Fix: add both to the null check; split into "basic" functions
//             (needed for the fallback glReadPixels path) vs. "PBO"
//             functions (needed only for the async path).  Introduce
//             m_gl_failed to unambiguously gate all subsequent GL calls.
//
//   Bug 4b — When loadGLFunctions() returned false the caller set
//             m_gl_loaded = true (to avoid flooding the log) but then fell
//             straight through to fn_glGetIntegerv() and the fallback
//             fn_glReadPixels() call without checking whether those
//             pointers were actually non-null.  Both calls crashed under
//             Wine when the functions couldn't be resolved.
//             Fix: check m_gl_failed immediately after the load attempt
//             and return early; guard fn_glGetIntegerv/fn_glReadBuffer
//             restore calls with the same flag.
//
//   Bug 6  — The 16-byte trampoline buffer was allocated
//             PAGE_EXECUTE_READWRITE and never had write permission
//             revoked.  Any heap/stack overflow that reached the trampoline
//             pages could silently overwrite the jump target.
//             Fix: call VirtualProtect(trampoline, 16, PAGE_EXECUTE_READ)
//             after writing the instruction bytes and flushing the icache.
// =========================================================================

#include "sfe/ogl_hook.hpp"
#include "sfe/logger.hpp"

#include <windows.h>
#include <cstring>

// OpenGL constants we need without pulling in the full GL headers.
// Values are from the OpenGL / WGL spec and never change.
namespace glconst {
    constexpr unsigned int PIXEL_PACK_BUFFER      = 0x88EB;
    constexpr unsigned int STREAM_READ            = 0x88E1;
    constexpr unsigned int READ_ONLY              = 0x88B8;
    constexpr unsigned int BGRA                   = 0x80E1;  // GL_BGRA_EXT
    constexpr unsigned int UNSIGNED_BYTE          = 0x1401;
    constexpr unsigned int BACK                   = 0x0405;
    constexpr unsigned int READ_BUFFER            = 0x0C02;
    constexpr unsigned int READ_FRAMEBUFFER_BINDING = 0x8CAA;
    constexpr unsigned int READ_FRAMEBUFFER         = 0x8CA8;
    constexpr unsigned int DRAW_FRAMEBUFFER         = 0x8CA9;
    constexpr unsigned int FRAMEBUFFER              = 0x8D40;
    constexpr unsigned int COLOR_ATTACHMENT0        = 0x8CE0;
    constexpr unsigned int PIXEL_PACK_BUFFER_BINDING = 0x88ED;
}

namespace sfe {

// -------------------------------------------------------------------------
// Static member definitions
// -------------------------------------------------------------------------
OGLHook*                     OGLHook::s_instance           = nullptr;
OGLHook::PFN_wglSwapBuffers  OGLHook::s_real_wglSwapBuffers = nullptr;

// -------------------------------------------------------------------------
// Global singleton
// -------------------------------------------------------------------------
OGLHook& getOGLHook() {
    static OGLHook instance;
    return instance;
}

// -------------------------------------------------------------------------
// Install
// -------------------------------------------------------------------------

bool OGLHook::install(VideoEncoder* encoder, FrameTagFn tag_fn, void* user) {
    if (m_installed) return true;

    m_encoder  = encoder;
    m_tag_fn   = tag_fn;
    m_tag_user = user;
    s_instance = this;

    // Locate wglSwapBuffers in opengl32.dll.
    HMODULE hGL = GetModuleHandleA("opengl32.dll");
    if (!hGL) {
        sfe::log("OGLHook: opengl32.dll not loaded — cannot hook");
        return false;
    }

    void* target = reinterpret_cast<void*>(
        GetProcAddress(hGL, "wglSwapBuffers"));
    if (!target) {
        sfe::log("OGLHook: GetProcAddress(wglSwapBuffers) failed");
        return false;
    }
    sfe::log("OGLHook: wglSwapBuffers at %p", target);

    // Save original 5 bytes for restoration.
    m_patch_site = target;
    memcpy(m_original_bytes, target, 5);

    // Build 5-byte relative JMP to HookedSwapBuffers.
    //   E9 xx xx xx xx   JMP rel32
    uint8_t patch[5] = { 0xE9, 0, 0, 0, 0 };
    intptr_t rel = reinterpret_cast<intptr_t>(&HookedSwapBuffers)
                 - reinterpret_cast<intptr_t>(target) - 5;
    memcpy(patch + 1, &rel, 4);

    // Make the page writable, patch, restore protection.
    DWORD old_prot = 0;
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old_prot)) {
        sfe::log("OGLHook: VirtualProtect failed (GLE=%lu)", GetLastError());
        return false;
    }
    memcpy(target, patch, 5);
    VirtualProtect(target, 5, old_prot, &old_prot);
    FlushInstructionCache(GetCurrentProcess(), target, 5);

    // -----------------------------------------------------------------------
    // Build a callable trampoline for the real wglSwapBuffers:
    //   copy the 5 original bytes into a small executable buffer,
    //   then append a JMP back to byte 5 of the original function.
    //
    // We allocate 16 bytes (5 original + 5 JMP + slack).
    // Allocation starts W+X so we can write the bytes, then we demote
    // to X-only (W^X) once writing is done.  Keeping it writeable
    // permanently (as the original did) means any adjacent overflow can
    // silently overwrite the jump target.  (Bug 6)
    // -----------------------------------------------------------------------
    void* trampoline = VirtualAlloc(nullptr, 16,
                                    MEM_COMMIT | MEM_RESERVE,
                                    PAGE_EXECUTE_READWRITE);
    if (!trampoline) {
        sfe::log("OGLHook: VirtualAlloc for trampoline failed");
        // Restore the patch site before returning failure.
        VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old_prot);
        memcpy(target, m_original_bytes, 5);
        VirtualProtect(target, 5, old_prot, &old_prot);
        return false;
    }

    uint8_t* tb = reinterpret_cast<uint8_t*>(trampoline);
    memcpy(tb, m_original_bytes, 5); // original bytes
    tb[5] = 0xE9;                    // JMP
    intptr_t tramp_rel = reinterpret_cast<intptr_t>(target) + 5
                       - reinterpret_cast<intptr_t>(tb + 5) - 5;
    memcpy(tb + 6, &tramp_rel, 4);
    FlushInstructionCache(GetCurrentProcess(), trampoline, 16);

    // Bug 6 fix: demote to execute-only — no further writes needed.
    DWORD tramp_old = 0;
    if (!VirtualProtect(trampoline, 16, PAGE_EXECUTE_READ, &tramp_old)) {
        // Non-fatal: log it but keep going — the trampoline is still callable.
        sfe::log("OGLHook: WARNING — could not demote trampoline to PAGE_EXECUTE_READ "
                 "(GLE=%lu)", GetLastError());
    }

    s_real_wglSwapBuffers = reinterpret_cast<PFN_wglSwapBuffers>(trampoline);

    m_installed = true;
    sfe::log("OGLHook: wglSwapBuffers hooked successfully");
    return true;
}

// -------------------------------------------------------------------------
// Uninstall
// -------------------------------------------------------------------------

void OGLHook::uninstall() {
    if (!m_installed) return;

    // Release PBOs before the GL context disappears.
    releasePBOs();

    // Restore the original 5 bytes at wglSwapBuffers.
    if (m_patch_site) {
        DWORD old_prot = 0;
        VirtualProtect(m_patch_site, 5, PAGE_EXECUTE_READWRITE, &old_prot);
        memcpy(m_patch_site, m_original_bytes, 5);
        VirtualProtect(m_patch_site, 5, old_prot, &old_prot);
        FlushInstructionCache(GetCurrentProcess(), m_patch_site, 5);
    }

    // Free the trampoline buffer.
    if (s_real_wglSwapBuffers) {
        VirtualFree(reinterpret_cast<void*>(s_real_wglSwapBuffers), 0, MEM_RELEASE);
        s_real_wglSwapBuffers = nullptr;
    }

    m_installed = false;
    s_instance  = nullptr;
    sfe::log("OGLHook: uninstalled");
}

// -------------------------------------------------------------------------
// GL function loader
// -------------------------------------------------------------------------
//
// Two-tier null check (Bug 4a fix):
//
//   Tier 1 — "basic" functions: glReadPixels, glReadBuffer, glGetIntegerv,
//             glGetError.  These are standard GL 1.x exports from opengl32.dll
//             and are needed even for the synchronous fallback path.  If any
//             of these is missing, capture is impossible and m_gl_failed is
//             set to true so every GL call site can skip safely.
//
//   Tier 2 — "PBO" functions: glGenBuffers … glBindFramebuffer.  These are
//             ARB extensions resolved via wglGetProcAddress.  If they are
//             absent (very old driver or stripped Wine build) we fall back
//             to synchronous glReadPixels without setting m_gl_failed.
// -------------------------------------------------------------------------

bool OGLHook::loadGLFunctions() {
    // m_gl_attempted gates this so we run exactly once (suppress log spam).
    if (m_gl_attempted) return !m_gl_failed;
    m_gl_attempted = true;

    HMODULE hGL = GetModuleHandleA("opengl32.dll");
    if (!hGL) {
        sfe::log("OGLHook: opengl32.dll not loaded during GL function init");
        m_gl_failed = true;
        return false;
    }

    // --- Tier 1: basic functions (opengl32.dll exports) ------------------
    fn_glReadPixels  = reinterpret_cast<PFN_glReadPixels> (GetProcAddress(hGL, "glReadPixels"));
    fn_glReadBuffer  = reinterpret_cast<PFN_glReadBuffer> (GetProcAddress(hGL, "glReadBuffer"));
    fn_glGetIntegerv = reinterpret_cast<PFN_glGetIntegerv>(GetProcAddress(hGL, "glGetIntegerv"));
    fn_glGetError    = reinterpret_cast<PFN_glGetError>   (GetProcAddress(hGL, "glGetError"));

    // Bug 4a fix: fn_glGetError is a basic function — include it in the
    // mandatory null check.  The original code loaded it but never checked it,
    // so a failed resolve left a null pointer called from prepareReadBuffer().
    if (!fn_glReadPixels || !fn_glReadBuffer || !fn_glGetIntegerv || !fn_glGetError) {
        sfe::log("OGLHook: basic GL functions missing — capture disabled "
                 "(glReadPixels=%p glReadBuffer=%p glGetIntegerv=%p glGetError=%p)",
                 reinterpret_cast<void*>(fn_glReadPixels),
                 reinterpret_cast<void*>(fn_glReadBuffer),
                 reinterpret_cast<void*>(fn_glGetIntegerv),
                 reinterpret_cast<void*>(fn_glGetError));
        m_gl_failed = true;
        return false;
    }

    // --- Tier 2: PBO / extension functions (wglGetProcAddress) -----------
    auto wgpa = reinterpret_cast<PROC(WINAPI*)(LPCSTR)>(
        GetProcAddress(hGL, "wglGetProcAddress"));
    if (!wgpa) {
        sfe::log("OGLHook: wglGetProcAddress not found — PBO path disabled, "
                 "using synchronous glReadPixels fallback");
        // Not fatal: fallback path uses only the Tier-1 functions above.
        return true;
    }

    fn_glGenBuffers      = reinterpret_cast<PFN_glGenBuffers>    (wgpa("glGenBuffers"));
    fn_glDeleteBuffers   = reinterpret_cast<PFN_glDeleteBuffers> (wgpa("glDeleteBuffers"));
    fn_glBindBuffer      = reinterpret_cast<PFN_glBindBuffer>    (wgpa("glBindBuffer"));
    fn_glBufferData      = reinterpret_cast<PFN_glBufferData>    (wgpa("glBufferData"));
    fn_glMapBuffer       = reinterpret_cast<PFN_glMapBuffer>     (wgpa("glMapBuffer"));
    fn_glUnmapBuffer     = reinterpret_cast<PFN_glUnmapBuffer>   (wgpa("glUnmapBuffer"));
    fn_glBindFramebuffer = reinterpret_cast<PFN_glBindFramebuffer>(wgpa("glBindFramebuffer"));

    // Bug 4a fix: fn_glBindFramebuffer was loaded but not null-checked in the
    // original.  It is called inside prepareReadBuffer() via the FBO path; a
    // null pointer here caused an access violation the first time an FBO was
    // active.  (fn_glBindFramebuffer is optional — absence means no FBO
    // support, which is fine; createPBOs / onBeforeSwap already guard on
    // fn_glGenBuffers being non-null before entering the PBO path.)
    if (!fn_glGenBuffers  || !fn_glDeleteBuffers ||
        !fn_glBindBuffer  || !fn_glBufferData    ||
        !fn_glMapBuffer   || !fn_glUnmapBuffer   ||
        !fn_glBindFramebuffer) {
        sfe::log("OGLHook: one or more PBO/extension functions not found "
                 "— using synchronous glReadPixels fallback");
        // Clear them all so fn_glGenBuffers == nullptr gates PBO creation.
        fn_glGenBuffers      = nullptr;
        fn_glDeleteBuffers   = nullptr;
        fn_glBindBuffer      = nullptr;
        fn_glBufferData      = nullptr;
        fn_glMapBuffer       = nullptr;
        fn_glUnmapBuffer     = nullptr;
        fn_glBindFramebuffer = nullptr;
        return true; // fallback path still works
    }

    m_gl_loaded = true;
    sfe::log("OGLHook: GL functions loaded (PBO path active)");
    return true;
}

// -------------------------------------------------------------------------
// PBO management
// -------------------------------------------------------------------------

bool OGLHook::createPBOs() {
    fn_glGenBuffers(PBO_COUNT, m_pbos);
    for (int i = 0; i < PBO_COUNT; ++i) {
        fn_glBindBuffer(glconst::PIXEL_PACK_BUFFER, m_pbos[i]);
        fn_glBufferData(glconst::PIXEL_PACK_BUFFER,
                        FRAME_BUFFER_SIZE,
                        nullptr,
                        glconst::STREAM_READ);
        m_pbo_ready[i] = false;
    }
    fn_glBindBuffer(glconst::PIXEL_PACK_BUFFER, 0);
    m_pbos_created  = true;
    m_pbo_write_idx = 0;
    sfe::log("OGLHook: %d PBOs created (%d MB total)",
             PBO_COUNT, PBO_COUNT * FRAME_BUFFER_SIZE / (1024 * 1024));
    return true;
}

bool OGLHook::releasePBOs() {
    if (!m_pbos_created || !fn_glDeleteBuffers) return false;
    fn_glDeleteBuffers(PBO_COUNT, m_pbos);
    memset(m_pbos, 0, sizeof(m_pbos));
    m_pbos_created = false;
    return true;
}

// -------------------------------------------------------------------------
// Per-frame capture (called on game thread before the actual swap)
// -------------------------------------------------------------------------

void OGLHook::onBeforeSwap() {
    if (!m_tag_fn) return;

    // Single call: advances the owner's state machine AND returns the tag for
    // the frame about to be presented.  Must happen before the pixel read so
    // frame N's pixels carry frame N's inputs.
    const FrameTag tag = m_tag_fn(m_tag_user);

    if (!tag.capture) return;
    if (!m_encoder) return;

    // ------------------------------------------------------------------
    // Lazy GL function load.
    //
    // Bug 4b fix: the original code set m_gl_loaded = true when
    // loadGLFunctions() returned false (to suppress per-frame log spam),
    // then fell through to fn_glGetIntegerv() without checking whether
    // those pointers were actually valid.  We now use m_gl_attempted /
    // m_gl_failed to separate "have we tried?" from "did it succeed?".
    // ------------------------------------------------------------------
    if (!m_gl_attempted) {
        if (!loadGLFunctions()) {
            // m_gl_failed is already set.  Log once then bail every frame.
            sfe::log("OGLHook: GL function load failed — capture disabled");
        }
    }

    // If even the basic functions are unavailable, there is nothing we can do.
    if (m_gl_failed) return;

    // Attempt PBO creation on the first capturing frame (only if we have the
    // extension functions available).
    if (!m_pbos_created && fn_glGenBuffers) {
        createPBOs();
    }

    // ------------------------------------------------------------------
    // Save / restore GL state to avoid corrupting wined3d's state machine.
    // Safe to call because m_gl_failed == false guarantees fn_glGetIntegerv
    // and fn_glReadBuffer are non-null (Tier-1 functions).
    // ------------------------------------------------------------------
    int prev_read_buf = 0;
    int prev_pbo      = 0;
    fn_glGetIntegerv(glconst::READ_BUFFER,              &prev_read_buf);
    fn_glGetIntegerv(glconst::PIXEL_PACK_BUFFER_BINDING, &prev_pbo);

    // ------------------------------------------------------------------
    // PBO path (preferred): async GPU → CPU DMA
    // ------------------------------------------------------------------
    if (m_pbos_created) {
        const int write_idx = m_pbo_write_idx;
        const int read_idx  = (write_idx + 1) % PBO_COUNT; // oldest initiated

        // Step 1: Initiate readback for this frame into the write PBO.
        //         glReadPixels with a bound PBO is asynchronous — it
        //         queues a DMA transfer and returns immediately.
        prepareReadBuffer();
        fn_glBindBuffer(glconst::PIXEL_PACK_BUFFER, m_pbos[write_idx]);
        fn_glReadPixels(0, 0, GAME_WIDTH, GAME_HEIGHT,
                        glconst::BGRA, glconst::UNSIGNED_BYTE,
                        nullptr); // offset 0 within the PBO
        m_pbo_ready[write_idx] = true;

        // Save metadata for this frame. It will be used when this PBO
        // is mapped and consumed in (PBO_COUNT-1) frames.  The tag travels
        // with the PBO precisely because the pixels lag the tag by that many
        // frames -- reading a "current" input at map time would mislabel every
        // frame by the pipeline depth.
        m_pbo_metadata[write_idx] = { tag.frame_index, tag.p1_input, tag.p2_input };

        // Step 2: Map and consume the oldest PBO (PBO_COUNT−1 frames old).
        //         By now the GPU DMA into that PBO is guaranteed complete.
        if (m_pbo_ready[read_idx]) {
            fn_glBindBuffer(glconst::PIXEL_PACK_BUFFER, m_pbos[read_idx]);
            void* ptr = fn_glMapBuffer(glconst::PIXEL_PACK_BUFFER,
                                       glconst::READ_ONLY);
            if (ptr) {
                // Acquire a ring slot (blocks if full — back-pressure).
                FrameSlot* slot = m_encoder->ring().acquireWriteSlot();
                if (slot) {
                    memcpy(slot->pixels, ptr, FRAME_BUFFER_SIZE);
                    const auto& meta = m_pbo_metadata[read_idx];
                    slot->frame_index = meta.frame_index;
                    slot->p1_input    = meta.p1;
                    slot->p2_input    = meta.p2;
                    m_encoder->ring().commitWriteSlot();
                }
                fn_glUnmapBuffer(glconst::PIXEL_PACK_BUFFER);
            } else {
                sfe::log("OGLHook: glMapBuffer returned null — skipping frame");
            }
            m_pbo_ready[read_idx] = false;
        }

        // Advance the write index for next frame.
        m_pbo_write_idx = (write_idx + 1) % PBO_COUNT;

    } else {
        // ------------------------------------------------------------------
        // Fallback path: synchronous glReadPixels directly into ring slot.
        //
        // Bug 4b fix: the original code reached here even when
        // fn_glReadPixels was null (because m_gl_failed wasn't tracked).
        // The early-return above now guarantees fn_glReadPixels != nullptr.
        // ------------------------------------------------------------------
        FrameSlot* slot = m_encoder->ring().acquireWriteSlot();
        if (slot) {
            prepareReadBuffer();
            fn_glReadPixels(0, 0, GAME_WIDTH, GAME_HEIGHT,
                            glconst::BGRA, glconst::UNSIGNED_BYTE,
                            slot->pixels);
            slot->frame_index = tag.frame_index;
            slot->p1_input    = tag.p1_input;
            slot->p2_input    = tag.p2_input;
            m_encoder->ring().commitWriteSlot();
        }
    }

    // Restore state.
    fn_glReadBuffer(prev_read_buf);
    fn_glBindBuffer(glconst::PIXEL_PACK_BUFFER, prev_pbo);
}

// -------------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------------

void OGLHook::prepareReadBuffer() {
    // Safe: caller (onBeforeSwap) already checked m_gl_failed == false,
    // guaranteeing fn_glGetIntegerv, fn_glReadBuffer, fn_glGetError are valid.
    int current_fb = 0;
    fn_glGetIntegerv(glconst::READ_FRAMEBUFFER_BINDING, &current_fb);

    if (current_fb != 0) {
        // If an FBO is bound, we read from its first color attachment.
        fn_glReadBuffer(glconst::COLOR_ATTACHMENT0);
    } else {
        // Default framebuffer. Some Wine configurations/drivers might not
        // have a BACK buffer if the context is single-buffered (unlikely
        // for Soku but possible).
        fn_glReadBuffer(glconst::BACK);

        // Check if that actually worked.
        if (fn_glGetError() != 0) {
            // If BACK failed, try FRONT as a last resort.
            fn_glReadBuffer(0x0404); // GL_FRONT
            fn_glGetError(); // Clear error
        }
    }
}

// -------------------------------------------------------------------------
// Hooked wglSwapBuffers  (static — called from patched JMP)
// -------------------------------------------------------------------------

int WINAPI OGLHook::HookedSwapBuffers(void* hdc) {
    // Capture before swap so we read the just-rendered backbuffer.
    if (s_instance) s_instance->onBeforeSwap();

    // Jump to the real function via the trampoline.
    return s_real_wglSwapBuffers(hdc);
}

} // namespace sfe