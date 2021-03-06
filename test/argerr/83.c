/* 
TEST_HEADER
 id = $Id$
 summary = UNALIGNED addr for ld_add
 language = c
 link = testlib.o
END_HEADER
*/

#include "testlib.h"
#include "arg.h"

static void test(void *stack_pointer)
{
 mps_arena_t arena;
 mps_ld_s ld;
 mps_thr_t thread;

 cdie(mps_arena_create(&arena, mps_arena_class_vm(), mmqaArenaSIZE), "create arena");

 cdie(mps_thread_reg(&thread, arena), "register thread");

 mps_ld_reset(&ld, arena);

 mps_ld_add(&ld, arena, UNALIGNED);
}

int main(void)
{
 run_test(test);
 return 0;
}
