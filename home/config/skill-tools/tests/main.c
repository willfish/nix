#include "run.h"
void run_tool_tests(void);
void run_audio_tests(void);
void run_token_tests(void);
int main(void) {
  run_tool_tests();
  run_audio_tests();
  run_token_tests();
  check_begin("skill-tools-checks");
  return check_finish();
}
