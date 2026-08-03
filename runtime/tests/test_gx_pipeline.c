#include "gx/gx.h"

#include <stdio.h>

static int check(int condition, const char* message) {
    if (condition)
        return 1;
    fprintf(stderr, "gx pipeline carry boundary failed: %s\n", message);
    return 0;
}

int main(void) {
    const u32 capacity = 128u;
    const u32 gather = 32u;

    if (!check(gcn_gx_pipeline_carry_can_resume(0u, capacity, gather),
               "empty carry must resume") ||
        !check(gcn_gx_pipeline_carry_can_resume(96u, capacity, gather),
               "96-byte carry must resume") ||
        !check(!gcn_gx_pipeline_carry_can_resume(97u, capacity, gather),
               "97-byte carry must defer") ||
        !check(!gcn_gx_pipeline_carry_can_resume(128u, capacity, gather),
               "full carry must defer") ||
        !check(!gcn_gx_pipeline_carry_can_resume(0u, 31u, gather),
               "gather larger than capacity must defer"))
        return 1;

    puts("gx pipeline carry boundary: ok");
    return 0;
}
