#include "simulator.h"

/* Thin entry point: forward CLI handling and execution to simulator_main. */
int main(int argc, char **argv) 
{
    return simulator_main(argc, argv);
}
