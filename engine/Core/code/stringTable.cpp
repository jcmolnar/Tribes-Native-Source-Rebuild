#include "types.h"
#include "base.h"
#include "stringTable.h"
#include "stdlib.h"

#ifdef _MSC_VER
#include <malloc.h>
#else
#include <alloc.h>
#endif

#define ST_INIT_SIZE 29

StringTable stringTable;

#ifdef __EMSCRIPTEN__
// WASM-PORT: defensive validity check for a StringTable bucket entry. A latent heap overflow
// elsewhere in the game-data path can clobber a Node's val pointer; strcasecmp on that wild
// pointer then traps "memory access out of bounds" (surfaced via CMDConsole::getVariable, which
// every script + the harness probe hits constantly). Desktop tolerated the wild read; wasm faults
// hard. Validate the Node and its val lie below the heap break before comparing; a bad entry is
// skipped (treated as a non-match) so the walk survives. This is a containment guard — the real
// fix is the upstream overflow — but it keeps the console (hence the whole game loop) alive.
extern "C" void* sbrk(long);
// Heap top, refreshed once per insert/lookup CALL (not per chain entry — sbrk per comparison
// tanked the frame rate, since getVariable runs thousands of times a frame). The heap only grows,
// so a value sampled at call start is a safe upper bound for that call's walk.
static inline uintptr_t stHeapTop() { return (uintptr_t)sbrk(0); }
static inline bool stValid(const void* p, uintptr_t top)
{
   uintptr_t a = (uintptr_t)p;
   return p != 0 && a >= 1024 && a < top;
}
#endif

//---------------------------------------------------------------
//
// DataChunker functions
//
//---------------------------------------------------------------

DataChunker::DataChunker(int size)
{
   chunkSize = size;
   curBlock  = new DataBlock(size);
   curBlock->next = NULL;
   curBlock->curIndex = 0;
}

DataChunker::~DataChunker()
{
#ifdef _MSC_VER
   int hc = _heapchk();
   AssertFatal((hc == _HEAPOK) || (hc == _HEAPEMPTY), "Heap error.");
#else
   int hc = heapcheck();
   AssertFatal(hc != _HEAPCORRUPT, "Heap error.");
#endif
   freeBlocks();
}

void *DataChunker::alloc(int size)
{
   AssertFatal(size <= chunkSize, "Data chunk too large.");
   if(!curBlock || size + curBlock->curIndex > chunkSize)
   {
      DataBlock *temp = new DataBlock(chunkSize);
      temp->next = curBlock;
      temp->curIndex = 0;
      curBlock = temp;
   }
   void *ret = curBlock->data + curBlock->curIndex;
   curBlock->curIndex += (size + 3) & ~3; // dword align
   return ret;
}

DataChunker::DataBlock::DataBlock(int size)
{
   data = new BYTE[size];
}

DataChunker::DataBlock::~DataBlock()
{
   delete[] data;
}

void DataChunker::freeBlocks()
{
   while(curBlock)
   {
      DataBlock *temp = curBlock->next;
      delete curBlock;
      curBlock = temp;
   }
}

//---------------------------------------------------------------
//
// StringTable functions
//
//---------------------------------------------------------------

unsigned int HashString(const char *str)
{
	unsigned int ret = 0;
   char c;
   while((c = *str++) != 0) {
      c = tolower(c);
		ret <<= 1;
		ret ^= (c * c);
	}
	return ret;
}

StringTable::StringTable()
{
	int i;
	
	buckets = (Node **) malloc(ST_INIT_SIZE * sizeof(Node *));
	for(i = 0; i < ST_INIT_SIZE; i++) {
		buckets[i] = 0;
	}
	numBuckets = ST_INIT_SIZE;
	itemCount = 0;
}

StringTable::~StringTable()
{
#ifdef _MSC_VER
   int hc = _heapchk();
   AssertFatal((hc == _HEAPOK) || (hc == _HEAPEMPTY), "Heap error.");
#else
   int hc = heapcheck();
   AssertFatal(hc != _HEAPCORRUPT, "Heap error.");
#endif
   free(buckets);
}

StringTableEntry StringTable::insert(const char * val, bool caseSens)
{
	Node **walk, *temp;
	unsigned int key = HashString(val);
	walk = &buckets[key % numBuckets];
#ifdef __EMSCRIPTEN__
   uintptr_t stTop = stHeapTop();
#endif
	while((temp = *walk) != NULL)	{
#ifdef __EMSCRIPTEN__
      // WASM-PORT: a clobbered chain (wild temp or temp->val) would trap in strcasecmp; if this
      // entry is not a valid pointer, terminate the chain here and insert fresh below.
      if (!stValid(temp, stTop) || !stValid(temp->val, stTop)) { *walk = NULL; break; }
#endif
      if(caseSens && !strcmp(temp->val, val))
         return temp->val;
      else if(!caseSens && !stricmp(temp->val, val))
   		return temp->val;
		walk = &(temp->next);
	}
	char *ret = 0;
	if(!*walk) {
		*walk = (Node *) mempool.alloc(sizeof(Node));
		(*walk)->next = 0;
      (*walk)->val = (char *) mempool.alloc(strlen(val) + 1);
      strcpy((*walk)->val, val);
		ret = (*walk)->val;
		itemCount ++;
	}
	if(itemCount > 2 * numBuckets) {
		resize(4 * numBuckets - 1);
	}
	return ret;
}

StringTableEntry StringTable::lookup(const char * val, bool caseSens)
{
	Node **walk, *temp;
	unsigned int key = HashString(val);
	walk = &buckets[key % numBuckets];
#ifdef __EMSCRIPTEN__
   uintptr_t stTop = stHeapTop();
#endif
	while((temp = *walk) != NULL)	{
#ifdef __EMSCRIPTEN__
      if (!stValid(temp, stTop) || !stValid(temp->val, stTop)) { *walk = NULL; break; }
#endif
      if(caseSens && !strcmp(temp->val, val))
            return temp->val;
      else if(!caseSens && !stricmp(temp->val, val))
   		return temp->val;
		walk = &(temp->next);
	}
   return NULL;
}

void StringTable::resize(int newSize)
{
	Node *head = NULL, *walk, *temp;
	int i;
   // reverse individual bucket lists
   // we do this because new strings are added at the end of bucket
   // lists so that case sens strings are always after their
   // corresponding case insens strings

	for(i = 0; i < numBuckets; i++) {
		walk = buckets[i];
      while(walk)
      {
         temp = walk->next;
         walk->next = head;
         head = walk;
         walk = temp;
      }
	}
	buckets = (Node **) realloc(buckets, newSize * sizeof(Node));
	for(i = 0; i < newSize; i++) {
		buckets[i] = 0;
	}
	numBuckets = newSize;
   walk = head;
	while(walk) {
		unsigned int key;
		Node *temp = walk;
		
		walk = walk->next;
		key = HashString(temp->val);
		temp->next = buckets[key % newSize];
		buckets[key % newSize] = temp;
	}
}

