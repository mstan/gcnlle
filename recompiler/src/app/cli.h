#ifndef DOLRECOMP_APP_CLI_H
#define DOLRECOMP_APP_CLI_H

#include <stddef.h>
#include "common/types.h"
#include "backend/emitter.h"

/* Extra flat segments recompiled alongside the primary IPL image into one
 * unified dispatch table (e.g. the BS2 exception handlers in low memory, which
 * live at a base disjoint from the stage-1/2 image). */
#define DOLRECOMP_MAX_SEGMENTS 8

typedef struct {
    const char* input_path;
    const char* title_id_arg;
    const char* output_arg;
    const char* symbol_suffix;   /* appended to emitted func_<addr> symbols */
    DolRecompCPU cpu;
    u32 jobs;
    u32 chunk_instructions;
    u32 rel_base;
    u32 ipl_base;
    u32 ipl_entry;
    u32 seg_base[DOLRECOMP_MAX_SEGMENTS];
    const char* seg_path[DOLRECOMP_MAX_SEGMENTS];
    u32 seg_count;
    int gamecube_mode;
    int cpu_explicit;
    int rel_base_set;
    int ipl_mode;
    int ipl_base_set;
    int ipl_entry_set;
    int setup_mode;
    int show_help;
} CliOptions;

void print_usage(const char* argv0);
int is_title_id(const char* text);
int is_title_id_length_valid(const char* text);
int parse_cpu_name(const char* text, DolRecompCPU* cpu);
const char* cpu_display_name(DolRecompCPU cpu);
void copy_title_id(char* out, size_t out_size, const char* title_id);
int parse_job_count(const char* text, u32* jobs);
int parse_chunk_instruction_count(const char* text, u32* count);
int parse_u32_arg(const char* text, const char* name, u32* value_out);
int parse_cli(int argc, char** argv, CliOptions* opts);

#endif
