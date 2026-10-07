#include "focus.h"
#include <locale.h>
int main(int argc, char **argv) {
  setlocale(LC_ALL, "");
  return focus_main(argc, argv);
}
