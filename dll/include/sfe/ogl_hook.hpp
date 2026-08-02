#pragma once
// =========================================================================
// SokuFrameExtractor — ogl_hook.hpp
// =========================================================================
// Hooks wglSwapBuffers (opengl32.dll) to capture the game's backbuffer
// via glReadPixels immediately before each Present, bypassing the window
// manager entirely.
//
// WHY THIS INSTEAD OF GetFrontBufferData
// ----------------------------------------
// GetFrontBufferData reads from the compositor's front buffer — i.e. the
// entire display surface.  Under Openbox (a reparenting WM), the game
// window may be offset, decorated, or clipped in ways that corrupt the
// crop region.
//
// glReadPixels called inside our wglSwapBuffers hook reads from the current
// GL context's own backbuffer, which is always exactly GAME_WIDTH ×
// GAME_HEIGHT pixels regardless of window position or WM decoration.
// There is no window-coordinate arithmetic and no compositor involvement.
//
// PBO DOUBLE-BUFFERING FOR ASYNC READBACK
// ----------------------------------------
// A naive glReadPixels stalls the CPU until the GPU pipeline drains.
// With PBO_COUNT rotating Pixel Buffer Objects:
//
//   Frame N  : glReadPixels → PBO[N % PBO_COUNT]   (async, returns immediately)
//   Frame N+1: glReadPixels → PBO[(N+1) % PBO_COUNT]
//   Frame N+2: glMapBuffer(PBO[(N) % PBO_COUNT]) → already complete → copy
//
// By the time we map a PBO, the GPU has had PBO_COUNT−1 additional frames
// to finish the DMA.  The game thread is never stalled waiting for the GPU.
//
// THREAD SAFETY
// -------------
// All OpenGL calls happen on the game thread (the thread that owns the GL
// context and calls wglSwapBuffers).  The ring-buffer write is also done
// on the game thread.  The encoder thread only reads from the ring buffer.
// No GL calls are made from the encoder thread.
// =========================================================================

#include "sfe/config.hpp"
#include "sfe/video_encoder.hpp"

namespace sfe {

// -------------------------------------------------------------------------
// What the owner tells us about the frame that is about to be presented.
// -------------------------------------------------------------------------
struct FrameTag {
    bool     capture;      // false -> skip this frame entirely
    int      frame_index;  // game tick this frame belongs to
    uint16_t p1_input;
    uint16_t p2_input;
};

// Called once per presented frame, on the game thread, BEFORE the pixels are
// read.  The callee is expected to advance its own state machine and return
// the tag describing the frame being presented right now.
//
// The ordering matters and is the whole reason this is a single call rather
// than the previous arrangement of a heartbeat plus three separately-written
// `staged_*` fields: pixels rendered for tick N must carry tick N's inputs.
// Previously that held only because OGLHook happened to call the FSM first and
// the FSM happened to write the staging fields before returning -- an
// invariant spread across two files with nothing naming it.  Now the tag is
// produced and consumed at one point, so it cannot drift.
using FrameTagFn = FrameTag (*)(void* user);

class OGLHook {
public:
    OGLHook()  = default;
    ~OGLHook() { uninstall(); }

    OGLHook(const OGLHook&)            = delete;
    OGLHook& operator=(const OGLHook&) = delete;

    // ------------------------------------------------------------------
    // Install / uninstall
    // ------------------------------------------------------------------

    // Patch wglSwapBuffers in opengl32.dll with a 5-byte JMP trampoline.
    // `encoder` must outlive this OGLHook.  `tag_fn` is invoked once per
    // presented frame with `user`; see FrameTagFn above.
    //
    // Taking a callback here is what breaks the old cycle: ogl_hook.cpp used
    // to #include frame_extractor.hpp to call getExtractor(), while
    // frame_extractor.cpp #included ogl_hook.hpp to write the staging fields.
    // Neither could be reasoned about, tested, or reused without the other.
    bool install(VideoEncoder* encoder, FrameTagFn tag_fn, void* user);

    // Restore the original 5 bytes and release PBOs.
    void uninstall();

private:
    // Called from the hooked wglSwapBuffers before the real swap.
    // Must be called on the thread that owns the GL context.
    void onBeforeSwap();
    void prepareReadBuffer();

    // Lazy GL function loader — resolves on first onBeforeSwap() call.
    // Returns true  → at least the basic functions loaded; capture is possible.
    // Returns false → critical functions missing; sets m_gl_failed = true,
    //                 all subsequent GL call sites must skip.
    bool loadGLFunctions();
    bool releasePBOs();
    bool createPBOs();

    VideoEncoder*  m_encoder   = nullptr;
    FrameTagFn     m_tag_fn    = nullptr;
    void*          m_tag_user  = nullptr;
    bool           m_installed = false;

    // Saved original bytes at wglSwapBuffers entry point (for restore).
    uint8_t m_original_bytes[5] = {};
    void*   m_patch_site        = nullptr;

    // Extension functions — must go through wglGetProcAddress.
    using PFN_glGenBuffers      = void  (WINAPI*)( int, unsigned int*);
    using PFN_glDeleteBuffers   = void  (WINAPI*)( int, const unsigned int*);
    using PFN_glBindBuffer      = void  (WINAPI*)( unsigned int, unsigned int);
    using PFN_glBufferData      = void  (WINAPI*)( unsigned int, ptrdiff_t, const void*, unsigned int);
    using PFN_glMapBuffer       = void* (WINAPI*)( unsigned int, unsigned int);
    using PFN_glUnmapBuffer     = unsigned char (WINAPI*)( unsigned int);
    using PFN_glBindFramebuffer = void  (WINAPI*)( unsigned int, unsigned int);

    // Standard GL functions from opengl32.dll (not needing wglGetProcAddress).
    using PFN_glReadPixels      = void (WINAPI*)( int, int, int, int, unsigned int, unsigned int, void*);
    using PFN_glReadBuffer      = void (WINAPI*)( unsigned int);
    using PFN_glGetIntegerv     = void (WINAPI*)( unsigned int, int*);
    using PFN_glGetError        = unsigned int (WINAPI*)(void);

    PFN_glGenBuffers      fn_glGenBuffers      = nullptr;
    PFN_glDeleteBuffers   fn_glDeleteBuffers   = nullptr;
    PFN_glBindBuffer      fn_glBindBuffer      = nullptr;
    PFN_glBufferData      fn_glBufferData      = nullptr;
    PFN_glMapBuffer       fn_glMapBuffer       = nullptr;
    PFN_glUnmapBuffer     fn_glUnmapBuffer     = nullptr;
    PFN_glBindFramebuffer fn_glBindFramebuffer = nullptr;
    PFN_glReadPixels      fn_glReadPixels      = nullptr;
    PFN_glReadBuffer      fn_glReadBuffer      = nullptr;
    PFN_glGetIntegerv     fn_glGetIntegerv     = nullptr;
    PFN_glGetError        fn_glGetError        = nullptr;

    // m_gl_attempted: true once loadGLFunctions() has run (suppresses spam).
    // m_gl_failed:    true when even the basic functions could not be loaded;
    //                 any code that dereferences a GL function pointer MUST
    //                 check this flag first.  This is distinct from m_gl_loaded
    //                 (which is true only on full success) so the fallback
    //                 glReadPixels path can also be skipped safely.
    bool m_gl_attempted = false;
    bool m_gl_failed    = false;
    bool m_gl_loaded    = false;  // true ↔ PBO path fully available


    // PBO ring: PBO_COUNT pixel buffer objects for async readback.
    unsigned int m_pbos[PBO_COUNT] = {};
    bool         m_pbo_ready[PBO_COUNT] = {}; // true once a readback was initiated

    struct PBOData {
        int      frame_index;
        uint16_t p1;
        uint16_t p2;
    } m_pbo_metadata[PBO_COUNT] = {};

    int          m_pbo_write_idx = 0;         // slot being written this frame
    bool         m_pbos_created  = false;

    // Pointer to the global OGLHook instance so the trampoline stub can
    // call back into us without global state coupling in the header.
    static OGLHook* s_instance;

    // The trampoline: our hooked function, calls s_instance->onBeforeSwap()
    // then jumps to the real wglSwapBuffers.
    static int WINAPI HookedSwapBuffers(void* hdc);

    // Pointer to the real wglSwapBuffers (called via trampoline bytes).
    using PFN_wglSwapBuffers = int (WINAPI*)( void* /*HDC*/);
    static PFN_wglSwapBuffers s_real_wglSwapBuffers;
};

/// Global singleton — FrameExtractor holds this as a member and exposes
/// staged_p1 / staged_p2 / staged_frame_index for the Process hook.
OGLHook& getOGLHook();

} // namespace sfe