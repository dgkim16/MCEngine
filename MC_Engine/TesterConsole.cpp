#include "TesterConsole.h"

#include "CBFreeList.h"

void ObjCBTest() {
	CBFreeList fl;
	fl.SetCapacity(10);
	assert(fl.Allocate() == 0);
	assert(fl.Allocate() == 1);
	fl.Release(0);
	assert(fl.Allocate() == 0);   // LIFO recycles 0
	assert(fl.Allocate() == 2);   // grows past 1
	assert(fl.HighWater() == 3);
}

void StartTesterConsole() {
	ObjCBTest();
}