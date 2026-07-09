/* gcnrecomp runtime — LLE host for the recompiled GameCube IPL.
 *
 * SKELETON. This does not boot the IPL yet; it exists so the runtime tree
 * builds green while the device models (see ../CMakeLists.txt) are written.
 * The real entry point will: map MEM1, load + descramble the IPL image,
 * hand control to the recompiled BS1/BS2/menu, and pump the block-dispatch
 * loop against faithful EXI/VI/GX/DSP/DI/SI models.
 */
#include <stdio.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    fprintf(stderr,
        "gcnrecomp runtime: skeleton build.\n"
        "Next: implement memory/cpu_glue/exi/vi/gx and load bios/ipl.bin.\n"
        "See docs/ROADMAP.md.\n");
    return 0;
}
