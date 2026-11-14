/*
 * BAD APPLE!!! for the Wii - created by Flashban9
 * Plays video.bin (1-bit frames) + audio.raw (32 kHz, 16-bit, stereo, big endian)
 * in an endless loop. The audio position is the master clock and the video follows it
 * (frames are skipped when needed, so the picture never lags behind the music).
 * HOME (Wiimote) / START (GC pad) / Reset: back to the Wii menu. Power: shut down.
 * A: change the aspect ratio (16:9 stretched / original 4:3 / 16:9 cropped).
 * (There is also a little easter egg. Find it. ;)
 */
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <asndlib.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/conf.h>
#include <ogc/machine/processor.h>
#include <fat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>

#include "decode.h"

#define SRC_W_MAX   320
#define SRC_H_MAX   240
#define AUDIO_RATE  32000
#define ABUF        8192        /* bytes per audio buffer (2048 sample frames = 64 ms) */
#define NBUF        8           /* about 0.5 s of queued audio to absorb SD card read hiccups */

#define COL_BLACK   0x10801080u
#define COL_WHITE   0xEB80EB80u

/* Two pixels per word (Y0 Cb Y1 Cr); index = (left bit << 1) | right bit */
static const u32 pair_col[4] = {
    COL_BLACK,        /* 00 */
    0x1080EB80u,   /* 01: black, white */
    0xEB801080u,   /* 10: white, black */
    COL_WHITE         /* 11 */
};

/* ---------- Globals ---------- */
static GXRModeObj *rmode;
static void *xfb[2];
static int fb_idx = 0;
static volatile int quit_flag = 0;

static FILE *vfile, *afile;
static u32 vw, vh, vfps, vframes;
static size_t frame_bytes;
static u8 *frame_bits;                       /* current bitmap frame */
static u8 vbuf[65536];
static long vdata_start;

/* Audio (streamed through ASND, fed by its own thread) */
static u8 abuf[NBUF][ABUF] ATTRIBUTE_ALIGN(32);
static u32 a_len[NBUF];
static int a_state[NBUF];                    /* 0 empty, 1 ready, 2 queued in ASND */
static int a_fill = 0;                       /* next buffer to fill */
static int a_sub = 0;                        /* next buffer to queue */
static u32 a_sub_count = 0;                  /* number of buffers queued so far */
static int a_started = 0;
static lwp_t audio_thread;
static volatile int audio_thread_run = 1;
/* Audio clock (written by the audio thread, read by the main loop) */
static volatile u64 clk_done = 0;            /* frames of buffers that finished playing */
static volatile u32 clk_cur = 0;             /* frames of the buffer that is playing now */
static volatile u32 clk_next = 0;            /* frames of the waiting buffer (0 = none) */
static volatile u64 clk_anchor = 0;          /* gettime() when the current buffer started */
static u64 loop_frames;                      /* sample frames per loop */

/* Render */
/* Display modes (button A cycles through them):
 *  0 = 16:9 fullscreen (picture is stretched over the whole screen)
 *  1 = original 4:3 (black bars left and right on a 16:9 TV)
 *  2 = 16:9 without distortion (a bit is cropped at the top and bottom) */
#define N_MODES 3
static int display_mode = 0;
static u16 xmap[640];
static u16 ymap[600];
static int dst_w, dst_h, dst_x_words, dst_y;
static u32 rowbuf[320] ATTRIBUTE_ALIGN(32);

/* ---------- Callbacks ---------- */
static void on_reset(u32 irq, void *ctx) { (void)irq; (void)ctx; quit_flag = 1; }
static void on_power(void) { quit_flag = 2; }

static void audio_refill(void)
{
    while (a_state[a_fill] == 0) {
        size_t n = fread(abuf[a_fill], 1, ABUF, afile);
        if (n == 0) { fseek(afile, 0, SEEK_SET); continue; }
        size_t len = n;
        if (n < ABUF) {                       /* end of file: pad to 32 bytes, then loop */
            len = (n + 31) & ~(size_t)31;
            memset(abuf[a_fill] + n, 0, len - n);
            fseek(afile, 0, SEEK_SET);
        }
        DCFlushRange(abuf[a_fill], ABUF);
        a_len[a_fill] = (u32)len;
        a_state[a_fill] = 1;
        a_fill = (a_fill + 1) % NBUF;
    }
}

/* Queues ready buffers in ASND. ASND always holds one playing and one waiting buffer. */
static void audio_feed(void)
{
    u32 level;
    s32 st = ASND_StatusVoice(0);

    if (a_state[a_sub] != 1) return;         /* nothing ready (underrun) */

    if (!a_started || st == SND_UNUSED) {    /* start, or restart after an underrun */
        u64 now = gettime();
        _CPU_ISR_Disable(level);
        clk_done += (u64)clk_cur + clk_next; /* everything queued so far has finished */
        clk_cur = a_len[a_sub] / 4;
        clk_next = 0;
        clk_anchor = now;
        _CPU_ISR_Restore(level);
        ASND_SetVoice(0, VOICE_STEREO_16BIT, AUDIO_RATE, 0, abuf[a_sub], a_len[a_sub], 255, 255, NULL);
        a_started = 1;
    } else if (st == SND_WAITING) {
        u64 now = gettime();
        _CPU_ISR_Disable(level);
        if (clk_next != 0) {                 /* the playing buffer ended, the next one is playing now */
            clk_done += clk_cur;
            clk_cur = clk_next;
            clk_anchor = now;
        }
        clk_next = a_len[a_sub] / 4;
        _CPU_ISR_Restore(level);
        ASND_AddVoice(0, abuf[a_sub], a_len[a_sub]);
    } else {
        return;                              /* both slots are in use */
    }

    a_state[a_sub] = 2;
    a_sub = (a_sub + 1) % NBUF;
    a_sub_count++;
    if (a_sub_count >= 4)                    /* only release buffers with a safety margin of extra buffers */
        a_state[(a_sub_count - 4) % NBUF] = 0;
}

/* Own thread: reads audio from the SD card and feeds ASND, independent of drawing */
static void *audio_thread_func(void *arg)
{
    (void)arg;
    while (audio_thread_run) {
        audio_refill();
        audio_feed();
        usleep(2000);
    }
    return NULL;
}

static u64 audio_position(void)
{
    u32 level;
    u64 pos, anchor, done;
    u32 cur;
    _CPU_ISR_Disable(level);
    done = clk_done;
    cur = clk_cur;
    anchor = clk_anchor;
    _CPU_ISR_Restore(level);
    if (!a_started) return 0;
    u64 us = ticks_to_microsecs(gettime() - anchor);
    u64 frames = (us * AUDIO_RATE) / 1000000ULL;
    if (frames > cur) frames = cur;          /* never beyond the end of the current buffer */
    pos = done + frames;
    return pos;
}

/* ---------- Video ---------- */
static void video_rewind(void)
{
    fseek(vfile, vdata_start, SEEK_SET);
    memset(frame_bits, 0, frame_bytes);
}

/* Decodes the next frame. 0 = ok, -1 = error/end */
static int video_next(void)
{
    u8 l[2];
    if (fread(l, 1, 2, vfile) != 2) return -1;
    size_t len = ((size_t)l[0] << 8) | l[1];
    if (len > sizeof(vbuf) || fread(vbuf, 1, len, vfile) != len) return -1;
    return decode_frame(vbuf, len, frame_bits, frame_bytes);
}

static void setup_geometry(void)
{
    int wide = (CONF_GetAspectRatio() == CONF_ASPECT_16_9);
    int fbw = rmode->fbWidth;
    int full_h = rmode->xfbHeight;
    int src_y0 = 0;
    int src_rows = vh;

    if (display_mode == 0) {                 /* fullscreen, stretched */
        dst_w = fbw;
        dst_h = full_h;
    } else if (display_mode == 1) {          /* original 4:3 */
        dst_w = wide ? (fbw * 3) / 4 : fbw;
        dst_h = full_h;
    } else {                                 /* 16:9 without distortion, cropped top and bottom */
        dst_w = fbw;
        dst_h = wide ? full_h : (full_h * 3) / 4;
        src_rows = (vh * 3) / 4;
        src_y0 = (vh - src_rows) / 2;
    }
    if (dst_w > 640) dst_w = 640;
    if (dst_h > 600) dst_h = 600;
    dst_w &= ~1;
    dst_x_words = ((fbw - dst_w) / 2) / 2;
    dst_y = (full_h - dst_h) / 2;
    for (int x = 0; x < dst_w; x++)
        xmap[x] = (u16)((x * vw) / dst_w);
    for (int y = 0; y < dst_h; y++)
        ymap[y] = (u16)(src_y0 + (y * src_rows) / dst_h);
}

static void render(void *fb)
{
    u32 *dst = (u32 *)fb;
    int pitch = rmode->fbWidth / 2;          /* words per line (2 pixels per word) */
    int words = dst_w / 2;
    size_t row_bytes = vw / 8;
    int last_sy = -1;

    for (int y = 0; y < dst_h; y++) {
        int sy = ymap[y];
        if (sy != last_sy) {                 /* only rebuild the row when the source row changes */
            const u8 *row = frame_bits + (size_t)sy * row_bytes;
            for (int w = 0; w < words; w++) {
                u32 x0 = xmap[2 * w], x1 = xmap[2 * w + 1];
                u32 b0 = (row[x0 >> 3] >> (7 - (x0 & 7))) & 1;
                u32 b1 = (row[x1 >> 3] >> (7 - (x1 & 7))) & 1;
                rowbuf[w] = pair_col[(b0 << 1) | b1];
            }
            last_sy = sy;
        }
        memcpy(dst + (size_t)(dst_y + y) * pitch + dst_x_words, rowbuf, (size_t)words * 4);
    }
}

static void clear_fb(void *fb)
{
    u32 *p = (u32 *)fb;
    size_t n = (size_t)(rmode->fbWidth / 2) * rmode->xfbHeight;
    for (size_t i = 0; i < n; i++) p[i] = COL_BLACK;
}

/* ---------- Easter egg: scrolling code view ---------- */
static const char *code_lines[] = {
    "#include <stdio.h>",
    "#include <stdlib.h>",
    "#include <string.h>",
    "",
    "typedef struct node { int key; struct node *l, *r; } node;",
    "",
    "static node *insert(node *t, int key)",
    "{",
    "    if (!t) {",
    "        t = calloc(1, sizeof(*t));",
    "        t->key = key;",
    "        return t;",
    "    }",
    "    if (key < t->key) t->l = insert(t->l, key);",
    "    else              t->r = insert(t->r, key);",
    "    return t;",
    "}",
    "",
    "static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)",
    "{",
    "    while (n--) {",
    "        crc ^= *p++;",
    "        for (int k = 0; k < 8; k++)",
    "            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));",
    "    }",
    "    return crc;",
    "}",
    "",
    "int main(int argc, char **argv)",
    "{",
    "    if (argc < 2) { fprintf(stderr, \"usage: %s <file>\\n\", argv[0]); return 1; }",
    "    FILE *f = fopen(argv[1], \"rb\");",
    "    uint8_t buf[4096];",
    "    size_t n;",
    "    uint32_t crc = 0xFFFFFFFFu;",
    "    while ((n = fread(buf, 1, sizeof buf, f)) > 0)",
    "        crc = crc32_update(crc, buf, n);",
    "    printf(\"crc32 = %08x\\n\", ~crc);",
    "    fclose(f);",
    "    return 0;",
    "}",
    "",
    "def fib(n):",
    "    a, b = 0, 1",
    "    for _ in range(n):",
    "        a, b = b, a + b",
    "    return a",
    "",
    "class Server(threading.Thread):",
    "    def run(self):",
    "        while self.alive:",
    "            conn, addr = self.sock.accept()",
    "            self.pool.submit(self.handle, conn, addr)",
    "",
    "for (i = 0; i < n; i++) {",
    "    ptr[i] = (ptr[i] << 3) ^ (ptr[i - 1] >> 5) ^ seed;",
    "    if (ptr[i] & 0x80000000) flags |= FLAG_OVERFLOW;",
    "}",
    "",
    "0x80003100: 7C0802A6  mflr r0",
    "0x80003104: 90010004  stw  r0, 4(r1)",
    "0x80003108: 9421FFE0  stwu r1, -32(r1)",
    "0x8000310C: 93E1001C  stw  r31, 28(r1)",
    "0x80003110: 38600000  li   r3, 0",
    "0x80003114: 4BFFF8ED  bl   _start",
    "",
    "SELECT u.id, COUNT(o.id) AS orders FROM users u",
    "  LEFT JOIN orders o ON o.user_id = u.id",
    " WHERE u.active = 1 GROUP BY u.id ORDER BY orders DESC;",
    "",
    "$ make -j4 && ./run --verbose --threads 8",
    "[ OK ] linking build/app.elf",
    "[ OK ] 0 errors, 0 warnings",
};
#define N_CODE_LINES ((int)(sizeof(code_lines) / sizeof(code_lines[0])))

static int code_mode = 0;
static int code_line = 0;

static void show_code_mode(void)
{
    VIDEO_ClearFrameBuffer(rmode, xfb[0], COLOR_BLACK);
    console_init(xfb[0], 20, 20, rmode->fbWidth - 20, rmode->xfbHeight - 20,
                 rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_SetNextFramebuffer(xfb[0]);
    VIDEO_Flush();
    fb_idx = 0;
    code_mode = 1;
}

static void show_video_mode(void)
{
    clear_fb(xfb[1]);
    VIDEO_SetNextFramebuffer(xfb[1]);
    VIDEO_Flush();
    fb_idx = 1;
    clear_fb(xfb[0]);
    code_mode = 0;
}

static void code_tick(void)
{
    /* mostly in order, now and then a jump -> looks like real scrolling */
    if (rand() % 12 == 0) code_line = rand() % N_CODE_LINES;
    else code_line = (code_line + 1) % N_CODE_LINES;
    printf("\x1b[32;1m%s\x1b[0m\n", code_lines[code_line]);
}

/* ---------- Startup / errors ---------- */
static void fatal(const char *msg)
{
    console_init(xfb[fb_idx], 20, 20, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_ClearFrameBuffer(rmode, xfb[fb_idx], COLOR_BLACK);
    printf("\x1b[2;0H\n  BAD APPLE!!!\n\n  %s\n\n  HOME = Exit\n", msg);
    VIDEO_SetNextFramebuffer(xfb[fb_idx]);
    VIDEO_Flush();
    while (!quit_flag) {
        WPAD_ScanPads();
        PAD_ScanPads();
        if ((WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) || (PAD_ButtonsDown(0) & PAD_BUTTON_START)) break;
        VIDEO_WaitVSync();
    }
    SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
    exit(0);
}

static int open_files(const char *argv0)
{
    char dirs[4][128];
    int nd = 0;
    if (argv0) {
        const char *colon = strchr(argv0, ':');
        const char *slash = strrchr(argv0, '/');
        if (colon && slash && slash > colon) {
            size_t n = (size_t)(slash - argv0) + 1;
            if (n < sizeof(dirs[0])) { memcpy(dirs[nd], argv0, n); dirs[nd][n] = 0; nd++; }
        }
    }
    strcpy(dirs[nd++], "sd:/apps/badapple/");
    strcpy(dirs[nd++], "usb:/apps/badapple/");
    strcpy(dirs[nd++], "fat:/apps/badapple/");

    for (int i = 0; i < nd; i++) {
        char p[160];
        snprintf(p, sizeof(p), "%svideo.bin", dirs[i]);
        vfile = fopen(p, "rb");
        if (!vfile) continue;
        setvbuf(vfile, NULL, _IOFBF, 32768);
        snprintf(p, sizeof(p), "%saudio.raw", dirs[i]);
        afile = fopen(p, "rb");
        if (afile) return 0;
        fclose(vfile);
        vfile = NULL;
    }
    return -1;
}

int main(int argc, char **argv)
{
    (void)argc;
    VIDEO_Init();
    CONF_Init();
    WPAD_Init();
    PAD_Init();
    SYS_SetResetCallback(on_reset);
    SYS_SetPowerCallback(on_power);

    rmode = VIDEO_GetPreferredMode(NULL);
    xfb[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    xfb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    VIDEO_Configure(rmode);
    clear_fb(xfb[0]);
    clear_fb(xfb[1]);
    VIDEO_SetNextFramebuffer(xfb[0]);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();

    if (!fatInitDefault()) fatal("Could not read the SD card / USB drive.");
    if (open_files(argc > 0 ? argv[0] : NULL) != 0)
        fatal("video.bin / audio.raw not found.\n  Folder: apps/badapple/ on SD or USB.");

    u8 hdr[14];
    if (fread(hdr, 1, 14, vfile) != 14 || memcmp(hdr, "BAD1", 4) != 0)
        fatal("video.bin is invalid.");
    vw = (hdr[4] << 8) | hdr[5];
    vh = (hdr[6] << 8) | hdr[7];
    vfps = (hdr[8] << 8) | hdr[9];
    vframes = ((u32)hdr[10] << 24) | (hdr[11] << 16) | (hdr[12] << 8) | hdr[13];
    if (vw > SRC_W_MAX || vh > SRC_H_MAX || (vw & 7) || vfps == 0 || vframes == 0)
        fatal("video.bin has an unexpected format.");
    vdata_start = ftell(vfile);
    frame_bytes = (size_t)vw * vh / 8;
    frame_bits = memalign(32, frame_bytes);
    memset(frame_bits, 0, frame_bytes);

    fseek(afile, 0, SEEK_END);
    long asize = ftell(afile);
    fseek(afile, 0, SEEK_SET);
    if (asize < ABUF) fatal("audio.raw is too small.");
    loop_frames = (((u64)asize + 31) & ~(u64)31) / 4;

    setup_geometry();

    /* Start audio */
    ASND_Init();
    ASND_Pause(0);
    audio_refill();
    LWP_CreateThread(&audio_thread, audio_thread_func, NULL, NULL, 16384, 80);

    long cur_loop = 0;
    long cur_frame = -1;
    int dirty = 0;
    int combo_prev = 0;
    u32 tick = 0;
    const u32 combo_mask = WPAD_BUTTON_1 | WPAD_BUTTON_2 | WPAD_BUTTON_UP;

    while (!quit_flag) {
        u64 pos = audio_position();
        long loop = (long)(pos / loop_frames);
        if (loop != cur_loop) {              /* audio started over -> video restarts too */
            cur_loop = loop;
            video_rewind();
            cur_frame = -1;
        }
        u64 local = pos - (u64)loop * loop_frames;
        long target = (long)((local * vfps) / AUDIO_RATE);
        if (target >= (long)vframes) target = (long)vframes - 1;

        while (cur_frame < target) {
            if (video_next() != 0) { cur_frame = target; break; }
            cur_frame++;
            dirty = 1;
        }

        if (dirty && !code_mode) {
            fb_idx ^= 1;
            render(xfb[fb_idx]);
            VIDEO_SetNextFramebuffer(xfb[fb_idx]);
            VIDEO_Flush();
            dirty = 0;
        }

        WPAD_ScanPads();
        PAD_ScanPads();
        if ((WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) || (PAD_ButtonsDown(0) & PAD_BUTTON_START))
            quit_flag = 1;

        if (!code_mode && (WPAD_ButtonsDown(0) & WPAD_BUTTON_A)) {
            display_mode = (display_mode + 1) % N_MODES;
            setup_geometry();
            clear_fb(xfb[0]);
            clear_fb(xfb[1]);
            dirty = 1;
        }

        int combo_now = (WPAD_ButtonsHeld(0) & combo_mask) == combo_mask;
        if (combo_now && !combo_prev) {
            if (code_mode) { show_video_mode(); dirty = 1; }
            else show_code_mode();
        }
        combo_prev = combo_now;

        if (code_mode && (++tick & 1) == 0) code_tick();

        VIDEO_WaitVSync();
    }

    audio_thread_run = 0;
    LWP_JoinThread(audio_thread, NULL);
    ASND_Pause(1);
    ASND_StopVoice(0);
    ASND_End();
    fclose(vfile);
    fclose(afile);
    VIDEO_SetBlack(TRUE);
    VIDEO_Flush();
    if (quit_flag == 2) SYS_ResetSystem(SYS_POWEROFF, 0, 0);
    SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
    return 0;
}
