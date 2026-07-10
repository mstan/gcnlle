#ifndef DOLRECOMP_IPL_H
#define DOLRECOMP_IPL_H

#include "../../common/types.h"

// Flat (headerless) firmware blob container.
//
// A descrambled GameCube BS2 payload has no section table: it is a raw
// big-endian PowerPC image meant to be loaded verbatim at a fixed guest
// address and entered at a single PC. This container wraps such a blob into
// the section/entry shape the split-C pipeline consumes (one code section
// spanning the whole image, plus the entry point).
//
// Default load base / entry for a USA BS2 payload. Both are overridable so
// the same path serves other regions/revisions and future flat images.
#define IPL_DEFAULT_BASE  0x81300000u
#define IPL_DEFAULT_ENTRY 0x81300000u

typedef struct {
    u8*  file_data;     // owned raw image bytes
    u32  file_size;     // bytes actually read from disk
    u32  code_size;     // file_size floored to a multiple of 4 (decodable words)
    u32  base_address;  // guest address the blob maps to at offset 0
    u32  entry_point;   // guest PC to begin execution at
} IPLFile;

// Load a flat blob and record where it maps and where execution starts.
// Validates alignment and that the entry point lands inside the image.
bool ipl_load(IPLFile* ipl, const char* path, u32 base_address, u32 entry_point);
void ipl_free(IPLFile* ipl);

// Dump load parameters to stdout (mirrors dol_print_info).
void ipl_print_info(const IPLFile* ipl, const char* image_name);

#endif /* DOLRECOMP_IPL_H */
