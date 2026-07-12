/* ================================================================================================
 * -*- C -*-
 * File: imgdump.c
 * Author: Guilherme R. Lampert
 * Created on: 26/10/15
 * Brief:
 *  Hardcoded command line tool that dumps PCX image files into
 *  C-style arrays of bytes, so that they can be easily embedded
 *  into a C program.
 *
 *  This tool is hardcoded to look for a few Quake2 textures in
 *  the CWD and dump them as arrays. These are then compiled into
 *  the PS2 executable to make it standalone. We only do that for
 *  basic texture like the conchars, the global palette and a couple
 *  others. Most of the code here was just copied from src/ps2/
 *
 * This source code is released under the GNU GPL v2 license.
 * Check the accompanying LICENSE file for details.
 * ================================================================================================ */

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  byte;
static u32 palette[256];

char * va(const char * format, ...)
{
    enum
    {
        MAX_BUF_CHARS = 2048
    };

    // Multiple buffers, in case called by nested functions.
    static int index = 0;
    static char strings[4][MAX_BUF_CHARS];

    char * bufptr = strings[index];
    index = (index + 1) & 3;

    va_list argptr;
    va_start(argptr, format);
    unsigned int charsWritten = vsnprintf(bufptr, MAX_BUF_CHARS, format, argptr);
    va_end(argptr);

    if (charsWritten >= MAX_BUF_CHARS)
    {
        charsWritten = MAX_BUF_CHARS - 1; // Truncate.
    }

    bufptr[charsWritten] = '\0'; // Ensure null terminated.
    return bufptr;
}

void Error(const char * msg)
{
    fprintf(stderr, "ERROR: %s\n", msg);
    exit(EXIT_FAILURE);
}

bool LoadBinaryFile(const char * filename, int * size_bytes, void ** data_ptr)
{
    FILE * fd;
    int file_len;
    int num_read;
    void * file_data = NULL;
    bool had_error = false;

    fd = fopen(filename, "rb");
    if (fd == NULL)
    {
        had_error = true;
        goto BAIL;
    }

    fseek(fd, 0, SEEK_END);
    file_len = ftell(fd);
    if (file_len <= 0)
    {
        had_error = true;
        goto BAIL;
    }

    rewind(fd);
    file_data = malloc(file_len);
    if (file_data == NULL)
    {
        had_error = true;
        goto BAIL;
    }

    num_read = fread(file_data, 1, file_len, fd);
    if (num_read != file_len)
    {
        had_error = true;
        goto BAIL;
    }

BAIL:

    if (fd != NULL)
    {
        fclose(fd);
    }

    if (had_error)
    {
        if (file_data != NULL)
        {
            free(file_data);
            file_data = NULL;
        }
        file_len = 0;
    }

    *size_bytes = file_len;
    *data_ptr   = file_data;
    return !had_error;
}

typedef struct
{
    char manufacturer;
    char version;
    char encoding;
    char bits_per_pixel;
    unsigned short xmin, ymin, xmax, ymax;
    unsigned short hres, vres;
    unsigned char palette[48];
    char reserved;
    char color_planes;
    unsigned short bytes_per_line;
    unsigned short palette_type;
    char filler[58];
    unsigned char data; // unbounded
} pcx_t;

bool PCX_LoadFromMemory(const char * filename, const byte * data, int data_len,
                        byte ** pic, u32 * palette, int * width, int * height)
{
    enum
    {
        PCX_PAL_SIZE_BYTES = 768
    };

    byte * pix;
    int x, y;
    int data_byte;
    int run_length;

    const pcx_t * pcx  = (const pcx_t *)data;
    int manufacturer   = pcx->manufacturer;
    int version        = pcx->version;
    int encoding       = pcx->encoding;
    int bits_per_pixel = pcx->bits_per_pixel;
    int xmax           = pcx->xmax;
    int ymax           = pcx->ymax;

    // Validate the image:
    if (manufacturer != 0x0A || version != 5 || encoding != 1 ||
        bits_per_pixel != 8  || xmax >= 640  || ymax >= 480)
    {
        Error("Bad PCX file. Invalid header value(s)!");
        return false;
    }

    // Get the palette:
    if (palette != NULL)
    {
        byte temp_pal[PCX_PAL_SIZE_BYTES] __attribute__((aligned(16)));
        memcpy(temp_pal, (const byte *)pcx + data_len - PCX_PAL_SIZE_BYTES, PCX_PAL_SIZE_BYTES);

        // Adjust the palette and fix byte order if needed:
        u32 c;
        int i, r, g, b;
        for (i = 0; i < 256; ++i)
        {
            r = temp_pal[(i * 3) + 0];
            g = temp_pal[(i * 3) + 1];
            b = temp_pal[(i * 3) + 2];
            c = (255 << 24) + (r << 0) + (g << 8) + (b << 16);
            palette[i] = c;
        }
        palette[255] &= 0xFFFFFF; // 255 is transparent
    }

    if (width != NULL)
    {
        *width = xmax + 1;
    }
    if (height != NULL)
    {
        *height = ymax + 1;
    }

    if (pic == NULL)
    {
        // Caller just wanted the palette.
        return true;
    }

    // Now alloc and read in the pixel data:
    pix = malloc((xmax + 1) * (ymax + 1));
    *pic = pix;

    // Skip the header:
    data = &pcx->data;

    // Decode the RLE packets:
    for (y = 0; y <= ymax; y++, pix += xmax + 1)
    {
        for (x = 0; x <= xmax;)
        {
            data_byte = *data++;
            if ((data_byte & 0xC0) == 0xC0)
            {
                run_length = data_byte & 0x3F;
                data_byte  = *data++;
            }
            else
            {
                run_length = 1;
            }

            while (run_length-- > 0)
            {
                pix[x++] = data_byte;
            }
        }
    }

    if ((data - (const byte *)pcx) > data_len)
    {
        Error("PCX image was malformed!");
        free(*pic);
        *pic = NULL;
        return false;
    }

    return true;
}

void Img_UnPalettize32(int width, int height, const byte * pic8in,
                       const u32 * palette, byte * pic32out)
{
    int i, p;
    const int pixel_count = width * height;
    u32 * rgba = (u32 *)pic32out;

    for (i = 0; i < pixel_count; ++i)
    {
        p = pic8in[i];
        rgba[i] = palette[p];

        if (p == 255)
        {
            // Transparent, so scan around for another
            // color to avoid alpha fringes
            if (i > width && pic8in[i - width] != 255)
            {
                p = pic8in[i - width];
            }
            else if (i < pixel_count - width && pic8in[i + width] != 255)
            {
                p = pic8in[i + width];
            }
            else if (i > 0 && pic8in[i - 1] != 255)
            {
                p = pic8in[i - 1];
            }
            else if (i < pixel_count - 1 && pic8in[i + 1] != 255)
            {
                p = pic8in[i + 1];
            }
            else
            {
                p = 0;
            }

            // Copy RGB components:
            ((byte *)&rgba[i])[0] = ((const byte *)&palette[p])[0];
            ((byte *)&rgba[i])[1] = ((const byte *)&palette[p])[1];
            ((byte *)&rgba[i])[2] = ((const byte *)&palette[p])[2];
        }
    }
}

void Img_UnPalettize16(int width, int height, const byte * pic8in,
                       const u32 * palette, byte * pic16out)
{
#define RGBA16(r, g, b, a) \
    (((a & 0x1) << 15) | (((b) >> 3) << 10) | (((g) >> 3) << 5) | ((r) >> 3))

    int i;
    u32 color;
    byte r, g, b, a;
    const int pixel_count = width * height;
    u16 * dest = (u16 *)pic16out;

    for (i = 0; i < pixel_count; ++i)
    {
        color = palette[pic8in[i]];

        r = (color & 0xFF);
        g = (color >>  8) & 0xFF;
        b = (color >> 16) & 0xFF;
        a = (color >> 24) & 0xFF;

        *dest++ = RGBA16(r, g, b, a);
    }
}

void DumpColormap(const u32 * palette)
{
    FILE * fd = fopen("palette.c", "wt");
    if (fd == NULL)
    {
        Error("Can't fopen palette.c!");
    }

    int i, j = 0;

    fprintf(fd, "\n// File generated by imgdump\n\n");

    fprintf(fd, "const unsigned int global_palette[256] __attribute__((aligned(16))) = {\n    ");
    for (i = 0; i < 256; ++i)
    {
        fprintf(fd, "0x%08X", palette[i]);

        if (i != (256 - 1))
        {
            fprintf(fd, ", ");
        }

        j++;
        if (j > 4)
        {
            fprintf(fd, "\n    ");
            j = 0;
        }
    }
    fprintf(fd, "\n};\n\n");

    fclose(fd);
}

void DumpImg(const byte * pic, int width, int height, int size_bytes,
             const char * filename, const char * tag)
{
    FILE * fd = fopen(filename, "wt");
    if (fd == NULL)
    {
        Error("Can't fopen file!");
    }

    int i, j = 0;

    fprintf(fd, "\n// File generated by imgdump\n\n");

    fprintf(fd, "const int %s_width  = %d;\n", tag, width);
    fprintf(fd, "const int %s_height = %d;\n", tag, height);
    fprintf(fd, "const int %s_size_bytes = %d;\n", tag, size_bytes);
    fprintf(fd, "const unsigned char %s_data[] __attribute__((aligned(16))) = {\n    ", tag);
    for (i = 0; i < size_bytes; ++i)
    {
        fprintf(fd, "0x%02X", pic[i]);

        if (i != (size_bytes - 1))
        {
            fprintf(fd, ", ");
        }

        j++;
        if (j > 14)
        {
            fprintf(fd, "\n    ");
            j = 0;
        }
    }
    fprintf(fd, "\n};\n\n");

    fclose(fd);
}

void DoImage(const char * name, int num_channels)
{
    int width = 0, height = 0;
    byte * pic = NULL;

    int size_bytes;
    byte * data_ptr;
    bool res = LoadBinaryFile(va("%s.pcx", name), &size_bytes, (void **)&data_ptr);
    if (!res)
    {
        Error(va("Failed to load %s.pcx!", name));
    }

    if (!PCX_LoadFromMemory("conchars.pcx", data_ptr, size_bytes,
                            &pic, NULL, &width, &height))
    {
        Error(va("Failed to load %s.pcx from memory!", name));
    }

    byte * pic_rgba = malloc(width * height * num_channels);

    if (num_channels == 4)
    {
        Img_UnPalettize32(width, height, pic, palette, pic_rgba);
    }
    else
    {
        Img_UnPalettize16(width, height, pic, palette, pic_rgba);
    }

    DumpImg(pic_rgba, width, height, (width * height * num_channels), va("%s.c", name), name);

    free(pic_rgba);
    free(pic);
}

int main()
{
    // colormap.pcx
    {
        int colormap_size;
        byte * colormap_data;
        bool res = LoadBinaryFile("colormap.pcx", &colormap_size, (void **)&colormap_data);
        if (!res)
        {
            Error("Failed to load colormap.pcx!");
        }

        if (!PCX_LoadFromMemory("colormap.pcx", colormap_data, colormap_size,
                                NULL, palette, NULL, NULL))
        {
            Error("Failed to load colormap.pcx from memory!");
        }

        DumpColormap(palette);
    }

    DoImage("conchars",  4);
    DoImage("conback",   2);
    DoImage("help",      2);
    DoImage("inventory", 2);
    DoImage("backtile",  2);

    printf("Done!\n");
}
