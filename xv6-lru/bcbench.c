#include "types.h"
#include "user.h"

int
main(int argc, char *argv[])
{
  int scan_blocks = 120;
  int rounds = 5;

  // Usage: bcbench [scan_blocks (1-300)] [rounds (1-20)]
  // NBUF itself is a compile-time kernel setting; use make NBUF=60, etc.
  if(argc > 1)
    scan_blocks = atoi(argv[1]);
  if(argc > 2)
    rounds = atoi(argv[2]);

  if(argc > 3 || scan_blocks < 1 || scan_blocks > 300 ||
     rounds < 1 || rounds > 20) {
    printf(2, "Usage: bcbench [scan_blocks: 1-300] [rounds: 1-20]\n");
    printf(2, "For a larger cache, rebuild with: make clean && make NBUF=60\n");
    exit();
  }

  bcachebench(scan_blocks, rounds);
  exit();
}
