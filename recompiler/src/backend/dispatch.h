#ifndef DOLRECOMP_BACKEND_DISPATCH_H
#define DOLRECOMP_BACKEND_DISPATCH_H

#include "common/types.h"
#include <stdio.h>

typedef struct {
    u32 start;
    u32 end;
} FunctionRange;

typedef struct {
    FunctionRange* ranges;
    u32 count;
    u32 capacity;
} FunctionList;

void emit_chunk_prototype(FILE* out, u32 func_addr);

/* Optional suffix appended to every emitted `func_<addr>` symbol.
 *
 * Exists so several recompiles of DIFFERENT code bodies that live at the SAME
 * guest address can be linked into one binary. That is the overlay/REL case:
 * a page such as 0x80F68000 hosts many different modules over a run, so the
 * address alone no longer identifies the code, and the content hash has to be
 * part of the symbol. Empty by default, which reproduces the historical names
 * byte for byte. */
void dolrecomp_set_symbol_suffix(const char* suffix);
const char* dolrecomp_symbol_suffix(void);
void function_list_free(FunctionList* list);
int function_list_add(FunctionList* list, u32 start, u32 end);
void emit_dispatch_helpers(FILE* out, const FunctionList* funcs, u32 entry_point);

#endif
