#include "gifenc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

/* helper to write a little-endian 16-bit number portably */
#define write_num(fd, n) write((fd), (uint8_t []) {(n) & 0xFF, (n) >> 8}, 2)

static uint8_t vga[0x30] = {
    0x00, 0x00, 0x00,
    0xAA, 0x00, 0x00,
    0x00, 0xAA, 0x00,
    0xAA, 0x55, 0x00,
    0x00, 0x00, 0xAA,
    0xAA, 0x00, 0xAA,
    0x00, 0xAA, 0xAA,
    0xAA, 0xAA, 0xAA,
    0x55, 0x55, 0x55,
    0xFF, 0x55, 0x55,
    0x55, 0xFF, 0x55,
    0xFF, 0xFF, 0x55,
    0x55, 0x55, 0xFF,
    0xFF, 0x55, 0xFF,
    0x55, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
};

struct Node {
    uint16_t key;
    struct Node *children[];
};
typedef struct Node Node;

static Node *
new_node(uint16_t key, int degree)
{
    Node *node = calloc(1, sizeof(*node) + degree * sizeof(Node *));
    if (node)
        node->key = key;
    return node;
}

static Node *
new_trie(int degree, int *nkeys)
{
    Node *root = new_node(0, degree);
    /* Create nodes for single pixels. */
    for (*nkeys = 0; *nkeys < degree; (*nkeys)++)
        root->children[*nkeys] = new_node(*nkeys, degree);
    *nkeys += 2; /* skip clear code and stop code */
    return root;
}

static void
del_trie(Node *root, int degree)
{
    if (!root)
        return;
    for (int i = 0; i < degree; i++)
        del_trie(root->children[i], degree);
    free(root);
}

#define write_and_store(s, dst, fd, src, n) \
do { \
    write(fd, src, n); \
    if (s) { \
        memcpy(dst, src, n); \
        dst += n; \
    } \
} while (0);

static void put_loop(ge_GIF *gif, uint16_t loop);

ge_GIF *
ge_new_gif(
    const char *fname, uint16_t width, uint16_t height,
    uint8_t *palette, int depth, int loop, int transparent_index
)
{
    int i, r, g, b, v;
    int store_gct, custom_gct;
    ge_GIF *gif = calloc(1, sizeof(*gif) + 2*width*height);
    if (!gif)
        goto no_gif;
    gif->w = width; gif->h = height;
    gif->transparent_index = transparent_index;
    gif->has_unencoded_frame = false;
    gif->frame = (uint8_t *) &gif[1];
    gif->back = &gif->frame[width*height];
#ifdef _WIN32
    gif->fd = creat(fname, S_IWRITE);
#else
    gif->fd = creat(fname, 0666);
#endif
    if (gif->fd == -1)
        goto no_fd;
#ifdef _WIN32
    setmode(gif->fd, O_BINARY);
#endif
    write(gif->fd, "GIF89a", 6);
    write_num(gif->fd, width);
    write_num(gif->fd, height);
    store_gct = custom_gct = 0;
    if (palette) {
        if (depth < 0)
            store_gct = 1;
        else
            custom_gct = 1;
    }
    if (depth < 0)
        depth = -depth;
    gif->depth = depth > 1 ? depth : 2;
    write(gif->fd, (uint8_t []) {0xF0 | (depth-1), 0x00, 0x00}, 3);
    if (custom_gct) {
        write(gif->fd, palette, 3 << depth);
    } else if (depth <= 4) {
        write_and_store(store_gct, palette, gif->fd, vga, 3 << depth);
    } else {
        write_and_store(store_gct, palette, gif->fd, vga, sizeof(vga));
        i = 0x10;
        for (r = 0; r < 6; r++) {
            for (g = 0; g < 6; g++) {
                for (b = 0; b < 6; b++) {
                    write_and_store(store_gct, palette, gif->fd,
                      ((uint8_t []) {r*51, g*51, b*51}), 3
                    );
                    if (++i == 1 << depth)
                        goto done_gct;
                }
            }
        }
        for (i = 1; i <= 24; i++) {
            v = i * 0xFF / 25;
            write_and_store(store_gct, palette, gif->fd,
              ((uint8_t []) {v, v, v}), 3
            );
        }
    }
done_gct:
    if (loop >= 0 && loop <= 0xFFFF)
        put_loop(gif, (uint16_t) loop);
    return gif;
no_fd:
    free(gif);
no_gif:
    return NULL;
}

static void
put_loop(ge_GIF *gif, uint16_t loop)
{
    write(gif->fd, (uint8_t []) {'!', 0xFF, 0x0B}, 3);
    write(gif->fd, "NETSCAPE2.0", 11);
    write(gif->fd, (uint8_t []) {0x03, 0x01}, 2);
    write_num(gif->fd, loop);
    write(gif->fd, "\0", 1);
}

/* Add packed key to buffer, updating offset and partial.
 *   gif->offset holds position to put next *bit*
 *   gif->partial holds bits to include in next byte */
static void
put_key(ge_GIF *gif, uint16_t key, int key_size)
{
    int byte_offset, bit_offset, bits_to_write;
    byte_offset = gif->offset / 8;
    bit_offset = gif->offset % 8;
    gif->partial |= ((uint32_t) key) << bit_offset;
    bits_to_write = bit_offset + key_size;
    while (bits_to_write >= 8) {
        gif->buffer[byte_offset++] = gif->partial & 0xFF;
        if (byte_offset == 0xFF) {
            write(gif->fd, "\xFF", 1);
            write(gif->fd, gif->buffer, 0xFF);
            byte_offset = 0;
        }
        gif->partial >>= 8;
        bits_to_write -= 8;
    }
    gif->offset = (gif->offset + key_size) % (0xFF * 8);
}

static void
end_key(ge_GIF *gif)
{
    int byte_offset;
    byte_offset = gif->offset / 8;
    if (gif->offset % 8)
        gif->buffer[byte_offset++] = gif->partial & 0xFF;
    if (byte_offset) {
        write(gif->fd, (uint8_t []) {byte_offset}, 1);
        write(gif->fd, gif->buffer, byte_offset);
    }
    write(gif->fd, "\0", 1);
    gif->offset = gif->partial = 0;
}

static void
put_image(ge_GIF *gif, uint8_t *frame, uint16_t w, uint16_t h, uint16_t x, uint16_t y)
{
    int nkeys, key_size, i, j;
    Node *node, *child, *root;
    int degree = 1 << gif->depth;

    write(gif->fd, ",", 1);
    write_num(gif->fd, x);
    write_num(gif->fd, y);
    write_num(gif->fd, w);
    write_num(gif->fd, h);
    write(gif->fd, (uint8_t []) {0x00, gif->depth}, 2);
    root = node = new_trie(degree, &nkeys);
    key_size = gif->depth + 1;
    put_key(gif, degree, key_size); /* clear code */
    for (i = y; i < y+h; i++) {
        for (j = x; j < x+w; j++) {
            uint8_t pixel = frame[i*gif->w+j] & (degree - 1);
            child = node->children[pixel];
            if (child) {
                node = child;
            } else {
                put_key(gif, node->key, key_size);
                if (nkeys < 0x1000) {
                    if (nkeys == (1 << key_size))
                        key_size++;
                    node->children[pixel] = new_node(nkeys++, degree);
                } else {
                    put_key(gif, degree, key_size); /* clear code */
                    del_trie(root, degree);
                    root = node = new_trie(degree, &nkeys);
                    key_size = gif->depth + 1;
                }
                node = root->children[pixel];
            }
        }
    }
    put_key(gif, node->key, key_size);
    put_key(gif, degree + 1, key_size); /* stop code */
    end_key(gif);
    del_trie(root, degree);
}

static int
get_bbox(ge_GIF *gif, uint16_t *w, uint16_t *h, uint16_t *x, uint16_t *y)
{
    int i, j, k;
    int left, right, top, bottom;
    left = gif->w; right = 0;
    top = gif->h; bottom = 0;
    k = 0;
    for (i = 0; i < gif->h; i++) {
        for (j = 0; j < gif->w; j++, k++) {
            if (gif->frame[k] != gif->back[k]) {
                if (j < left)   left    = j;
                if (j > right)  right   = j;
                if (i < top)    top     = i;
                if (i > bottom) bottom  = i;
            }
        }
    }
    if (left != gif->w && top != gif->h) {
        *x = left; *y = top;
        *w = right - left + 1;
        *h = bottom - top + 1;
        return 1;
    } else {
        return 0;
    }
}

typedef enum {
    DM_UNSPEC = 0 << 2,
    DM_DND    = 1 << 2, /* Do Not Dispose */
    DM_RTB    = 2 << 2, /* Restore To Background (clear pixel) */
    DM_RTP    = 3 << 2  /* Restore To Previous (not currently used) */
} DisposalMethod;

static void
add_graphics_control_extension(ge_GIF *gif, const uint16_t d, const DisposalMethod dm)
{
    uint8_t out[4] = {'!', 0xF9, 0x04, dm};
    if(gif->transparent_index != -1) {
        out[3] |= 0x1; /* transparent color flag */
    }
    write(gif->fd, out, sizeof(out));
    write_num(gif->fd, d);
    out[0] = 0;
    out[1] = 0;
    if(gif->transparent_index != -1) {
        out[0] = gif->transparent_index;
    }
    write(gif->fd, out, 2);
}

static void
add_frame_with_transparency(ge_GIF *gif, const bool has_new_frame)
{
    gif->has_unencoded_frame = false;
    DisposalMethod dm = DM_DND;
    uint16_t w = gif->unencoded_w;
    uint16_t h = gif->unencoded_h;
    uint16_t x = gif->unencoded_x;
    uint16_t y = gif->unencoded_y;
    if(has_new_frame) {
        /* if the new frame has any new transparent pixels (not already transparent) RTB is required */
        for(int i = 0; i < gif->h; i++) {
            for(int j = 0; j < gif->w; j++) {
                if((gif->frame[i*gif->w + j] == gif->transparent_index) && (gif->back[i*gif->w+j] != gif->transparent_index)) {
                    dm = DM_RTB;
                    /* adjust the BB so the pixel will be cleared on RTB*/
                    if(i < y) {
                        const uint16_t delta = y-i;
                        y = i;
                        h += delta;
                    }

                    if(j < x) {
                        const uint16_t delta = x-j;
                        x = j;
                        w += delta;
                    }

                    if(i >= (y+gif->h)) {
                        h += (i-(y+gif->h)+1);
                    }

                    if(j >= (x+gif->w)) {
                        w += (j-(x+gif->w)+1);
                    }
                }
            }
        }

    }
    add_graphics_control_extension(gif, gif->unencoded_delay, dm);
    put_image(gif, gif->back, w, h, x, y);

    if(dm == DM_RTB) {
        /* RTB our internal model, used by get_bbox*/
        for(int i = y; i < (y+h); i++) {
            for(int j = x; j < (x+w); j++) {
                gif->back[i*gif->w + j] = gif->transparent_index;
            }
        }
    }
}

void
ge_add_frame(ge_GIF *gif, uint16_t delay)
{
    uint16_t w, h, x, y;
    uint8_t *tmp;

    /* encode an old frame if needed */
    if(gif->has_unencoded_frame) {
        add_frame_with_transparency(gif, true);
    }

    /* determine the changed area since the last frame */
    if ((gif->nframes == 0)) {
        w = gif->w;
        h = gif->h;
        x = y = 0;
    } else if (!get_bbox(gif, &w, &h, &x, &y)) {
        /* image's not changed; save one pixel just to add delay */
        w = h = 1;
        x = y = 0;
    }

    /* encode the frame now if transparency isn't used at all*/
    if(gif->transparent_index == -1) {
        if(delay) {
            add_graphics_control_extension(gif, delay, DM_DND);
        }
        put_image(gif, gif->frame, w, h, x, y);
    } else {
        gif->has_unencoded_frame = true;
        gif->unencoded_w = w;
        gif->unencoded_h = h;
        gif->unencoded_x = x;
        gif->unencoded_y = y;
        gif->unencoded_delay = delay;
    }

    gif->nframes++;
    tmp = gif->back;
    gif->back = gif->frame;
    gif->frame = tmp;
}

void
ge_close_gif(ge_GIF* gif)
{
    /* encode an old frame if needed */
    if(gif->has_unencoded_frame) {
        add_frame_with_transparency(gif, false);
    }
    write(gif->fd, ";", 1);
    close(gif->fd);
    free(gif);
}
