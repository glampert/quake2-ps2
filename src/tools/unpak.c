
/* ================================================================================================
 * -*- C -*-
 * File: unpak.c
 * Author: Guilherme R. Lampert
 * Created on: 26/10/15
 * Brief: Very simple command line tool to unpack a Quake 2 PAK archive into a normal directory.
 *
 * This source code is released under the GNU GPL v2 license.
 * Check the accompanying LICENSE file for details.
 * ================================================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

// For mkdir/stat
#include <sys/types.h>
#include <sys/stat.h>

/*
 * From Quake2:
 */

// Not enforced by this extractor. Was enforced by the game, we just warn.
#define MAX_FILES_IN_PAK 4096

// 4CC 'PACK'
#define PAK_HEADER_IDENT (('K' << 24) + ('C' << 16) + ('A' << 8) + 'P')

typedef struct
{
    char name[56];
    int filepos;
    int filelen;
} pak_file_t;

typedef struct
{
    int ident;
    int dirofs;
    int dirlen;
} pak_header_t;

/*
 * Extractor code:
 */

static void make_path(const char * path_ended_with_sep_or_filename)
{
    struct stat dir_stat;
    char dir_path[512];

    strncpy(dir_path, path_ended_with_sep_or_filename, sizeof(dir_path));
    char * pPath = dir_path;

    while (*pPath != '\0')
    {
        if (*pPath == '/' || *pPath == '\\')
        {
            *pPath = '\0';
            if (stat(dir_path, &dir_stat) != 0)
            {
                if (mkdir(dir_path, 0777) != 0)
                {
                    fprintf(stderr, "mkdir('%s', 0777) failed!\n", dir_path);
                }
            }
            else // Path already exists.
            {
                if (!S_ISDIR(dir_stat.st_mode))
                {
                    // Looks like there is a file with the same name as the directory.
                    fprintf(stderr, "Can't mkdir()! Path points to a file.\n");
                }
            }
            *pPath = '/';
        }
        ++pPath;
    }
}

static bool write_file(const char * name, const void * data, int len_bytes)
{
    // First might need the create the file path:
    make_path(name);

    FILE * out_file = fopen(name, "wb");
    if (out_file == NULL)
    {
        fprintf(stderr, "Can't fopen() the file! %s\n", name);
        return false;
    }

    fwrite(data, 1, len_bytes, out_file);
    fclose(out_file);
    return true; // Assume write went OK.
}

static bool extract_file(FILE * pak_file, const char * dest_dir_name,
                         const char * name, int file_pos, int len_bytes)
{
    void * buffer = malloc(len_bytes);
    if (buffer == NULL)
    {
        fprintf(stderr, "Out-of-memory in extract_file!\n");
        return false;
    }

    fseek(pak_file, file_pos, SEEK_SET);
    fread(buffer, 1, len_bytes, pak_file);

    if (ferror(pak_file))
    {
        fprintf(stderr, "Error reading file data block for %s!\n", name);
        free(buffer);
        return false;
    }

    char full_path_name[512];
    snprintf(full_path_name, sizeof(full_path_name), "%s/%s", dest_dir_name, name);

    bool result = write_file(full_path_name, buffer, len_bytes);
    free(buffer);

    return result;
}

static bool unpak(FILE * pak_file, const pak_header_t * pak_header, const char * dest_dir_name)
{
    pak_file_t * pak_file_entries;
    int num_files_in_pak = pak_header->dirlen / sizeof(pak_file_t);

    if (num_files_in_pak > MAX_FILES_IN_PAK)
    {
        fprintf(stderr, "Warning: MAX_FILES_IN_PAK exceeded!\n");
        // Allow it to continue.
    }

    pak_file_entries = malloc(num_files_in_pak * sizeof(pak_file_t));
    if (pak_file_entries == NULL)
    {
        fprintf(stderr, "Out-of-memory in unpak!\n");
        return false;
    }

    fseek(pak_file, pak_header->dirofs, SEEK_SET);
    fread(pak_file_entries, 1, pak_header->dirlen, pak_file);

    if (ferror(pak_file))
    {
        fprintf(stderr, "Error reading pak_file_entries block!\n");
        free(pak_file_entries);
        return false;
    }

    int i;
    for (i = 0; i < num_files_in_pak; ++i)
    {
        const pak_file_t * entry = &pak_file_entries[i];
        if (!extract_file(pak_file, dest_dir_name, entry->name, entry->filepos, entry->filelen))
        {
            fprintf(stderr, "Failed to extract pak entry '%s' #%d\n", entry->name, i);
            // Try another one...
        }
    }

    free(pak_file_entries);
    return true;
}

int main(int argc, const char * argv[])
{
    if (argc <= 1)
    {
        fprintf(stderr, "No filename!\n");
        printf("Usage: \n"
               " $ %s <file.pak>\n"
               "   Unpacks the whole archive to a directory with the same name as the input.\n"
               "   Internal file paths are preserved.\n",
               argv[0]);
        return EXIT_FAILURE;
    }

    const char * pak_name = argv[1];
    FILE * pak_file = fopen(pak_name, "rb");

    if (pak_file == NULL)
    {
        fprintf(stderr, "Can't fopen() the file! %s\n", pak_name);
        return EXIT_FAILURE;
    }

    pak_header_t pak_header;
    fread(&pak_header, 1, sizeof(pak_header), pak_file);

    if (pak_header.ident != PAK_HEADER_IDENT)
    {
        fprintf(stderr, "Bad file id for pak %s!\n", pak_name);
        fclose(pak_file);
        return EXIT_FAILURE;
    }

    char * ext_ptr;
    char dest_dir_name[512];
    strncpy(dest_dir_name, pak_name, sizeof(dest_dir_name));

    // Remove the file extension, if any:
    if ((ext_ptr = strrchr(dest_dir_name, '.')) != NULL)
    {
        *ext_ptr = '\0';
    }

    if (!unpak(pak_file, &pak_header, dest_dir_name))
    {
        fprintf(stderr, "Unable to successfully unpack archive %s!\n", pak_name);
        fclose(pak_file);
        return EXIT_FAILURE;
    }

    fclose(pak_file);
    // Success.
}
