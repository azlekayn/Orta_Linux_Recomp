/*
 * apu_sdl_audio.c - SDL2 backend for the APU's waveOut monitor (POSIX).
 *
 * The APU's monitor (src/apu/apu_core.c) hands finished 48 kHz stereo s16
 * buffers to waveOut with the usual Win32 double-buffering: it fills a
 * buffer, submits it, and waits for WHDR_DONE before reusing it. On Windows
 * that is XAudio2 or winmm. On Linux there was no output at all -- the
 * waveOut entry points were inert inline stubs, so the APU produced silence
 * however much the guest mixed.
 *
 * This implements the same six waveOut entry points over an SDL2 audio
 * device, using SDL's pull callback: the callback drains the submitted
 * WAVEHDRs in order, and marks each WHDR_DONE when it has been played. That
 * keeps the monitor's own back-pressure design intact -- it still waits on
 * WHDR_DONE, and it still unlocks its APU mutex while it waits. No change to
 * apu_core.c is needed.
 *
 * The Win32 WAVEHDR surface is reused as-is. dwBytesRecorded (unused for
 * playback) carries the callback's read offset within the current buffer.
 */

#if !defined(_WIN32)

#include "platform/xbox_winnt.h"
#include <SDL.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    SDL_AudioDeviceID dev;
    int               opened;
    WAVEHDR          *head, *tail;   /* submitted, played in order via lpNext */
} sdl_waveout;

static sdl_waveout g_wo;

/* SDL audio callback (its own thread; the device is locked around the list). */
static void sdl_fill(void *ud, Uint8 *stream, int len)
{
    sdl_waveout *w = (sdl_waveout *)ud;
    Uint8 *out = stream;
    int left = len;

    memset(stream, 0, len);            /* underrun -> silence */

    while (left > 0 && w->head) {
        WAVEHDR *h = w->head;
        DWORD off = h->dwBytesRecorded;
        DWORD avail = h->dwBufferLength - off;
        DWORD take = (DWORD)left < avail ? (DWORD)left : avail;

        memcpy(out, h->lpData + off, take);
        h->dwBytesRecorded = off + take;
        out  += take;
        left -= (int)take;

        if (h->dwBytesRecorded >= h->dwBufferLength) {
            h->dwFlags |= WHDR_DONE;
            h->dwFlags &= ~WHDR_INQUEUE;
            h->dwBytesRecorded = 0;
            w->head = h->lpNext;
            if (!w->head)
                w->tail = NULL;
        }
    }
}

MMRESULT waveOutOpen(HWAVEOUT *h, UINT id, const WAVEFORMATEX *f,
                     DWORD_PTR cb, DWORD_PTR inst, DWORD flags)
{
    SDL_AudioSpec want, have;
    SDL_AudioDeviceID dev;

    (void)id; (void)cb; (void)inst; (void)flags;
    if (!h || !f)
        return MMSYSERR_INVALPARAM;

    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[APU] SDL audio init failed: %s\n", SDL_GetError());
        return MMSYSERR_INVALPARAM;
    }

    memset(&want, 0, sizeof want);
    want.freq     = (int)f->nSamplesPerSec;
    want.format   = AUDIO_S16SYS;
    want.channels = (Uint8)f->nChannels;
    want.samples  = 1024;
    want.callback = sdl_fill;
    want.userdata = &g_wo;

    g_wo.head = g_wo.tail = NULL;
    g_wo.opened = 0;

    /* allowed_changes = 0: the APU mixes at a fixed 48 kHz s16 stereo, so a
     * device that cannot match it is a failure rather than a resample. */
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) {
        fprintf(stderr, "[APU] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return MMSYSERR_INVALPARAM;
    }

    g_wo.dev = dev;
    g_wo.opened = 1;
    SDL_PauseAudioDevice(dev, 0);

    fprintf(stderr, "[APU] SDL audio output open (%d Hz, %d ch, %d-sample, "
                    "SDL driver '%s')\n",
            have.freq, have.channels, have.samples, SDL_GetCurrentAudioDriver());
    *h = &g_wo;
    return MMSYSERR_NOERROR;
}

MMRESULT waveOutPrepareHeader(HWAVEOUT h, WAVEHDR *hdr, UINT sz)
{
    (void)h; (void)sz;
    if (hdr) {
        hdr->dwFlags &= ~(WHDR_DONE | WHDR_INQUEUE);
        hdr->dwBytesRecorded = 0;
        hdr->lpNext = NULL;
    }
    return MMSYSERR_NOERROR;
}

MMRESULT waveOutUnprepareHeader(HWAVEOUT h, WAVEHDR *hdr, UINT sz)
{
    (void)h; (void)hdr; (void)sz;
    return MMSYSERR_NOERROR;
}

MMRESULT waveOutWrite(HWAVEOUT h, WAVEHDR *hdr, UINT sz)
{
    (void)h; (void)sz;
    if (!g_wo.opened || !hdr)
        return MMSYSERR_INVALPARAM;

    SDL_LockAudioDevice(g_wo.dev);
    hdr->dwBytesRecorded = 0;
    hdr->dwFlags &= ~WHDR_DONE;
    hdr->dwFlags |=  WHDR_INQUEUE;
    hdr->lpNext = NULL;
    if (g_wo.tail)
        g_wo.tail->lpNext = hdr;
    else
        g_wo.head = hdr;
    g_wo.tail = hdr;
    SDL_UnlockAudioDevice(g_wo.dev);

    return MMSYSERR_NOERROR;
}

MMRESULT waveOutReset(HWAVEOUT h)
{
    WAVEHDR *p;
    (void)h;
    if (!g_wo.opened)
        return MMSYSERR_NOERROR;

    SDL_LockAudioDevice(g_wo.dev);
    for (p = g_wo.head; p; p = p->lpNext) {
        p->dwFlags |=  WHDR_DONE;
        p->dwFlags &= ~WHDR_INQUEUE;
        p->dwBytesRecorded = 0;
    }
    g_wo.head = g_wo.tail = NULL;
    SDL_UnlockAudioDevice(g_wo.dev);
    return MMSYSERR_NOERROR;
}

MMRESULT waveOutClose(HWAVEOUT h)
{
    (void)h;
    if (!g_wo.opened)
        return MMSYSERR_NOERROR;
    SDL_PauseAudioDevice(g_wo.dev, 1);
    SDL_CloseAudioDevice(g_wo.dev);
    g_wo.opened = 0;
    g_wo.dev = 0;
    g_wo.head = g_wo.tail = NULL;
    return MMSYSERR_NOERROR;
}

#endif /* !_WIN32 */
