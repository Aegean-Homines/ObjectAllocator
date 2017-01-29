#include "ObjectAllocator.h"
#include <iostream>

using std::cout;
using std::endl;

#define OUT_OF_LOGICAL_MEMORY_ERROR "Cannot allocate new page - max pages has been reached"
#define OUT_OF_PHYSICAL_MEMORY_ERROR "Cannot allocate new page - out of physical memory: " + std::string(e.what())

ObjectAllocator::ObjectAllocator(size_t ObjectSize, const OAConfig & config) : PageList_(NULL), FreeList_(NULL), myConfig(config)
{
	// Save each object's size
	myStats.ObjectSize_ = ObjectSize;

	// Calculate a page's total size
	// Total Object Size
	size_t totalObjectSizeInPage = myConfig.ObjectsPerPage_ * ObjectSize;
	// Total Padding Size
	size_t totalPaddingSizeInPage = myConfig.ObjectsPerPage_ * myConfig.PadBytes_ * 2; // Padding to the left and right
	// Total HeaderSize
	size_t totalHeaderSizeInPage = myConfig.HBlockInfo_.size_ * myConfig.ObjectsPerPage_; // One header per object
	// Alignment
	// For left alignment: One header, One pad-byte and one "Next" pointer
	unsigned int leftTotalSize = static_cast<unsigned int>(myConfig.HBlockInfo_.size_ + myConfig.PadBytes_ + sizeof(void*));
	myConfig.LeftAlignSize_ = myConfig.Alignment_ ? (leftTotalSize % myConfig.Alignment_) : 0;
	leftPageSectionSize = leftTotalSize + myConfig.LeftAlignSize_;
	// For inter alignment = One header, two pad-bytes (after object and before next object) and object itself
	unsigned int interTotalSize = static_cast<unsigned int>(myConfig.HBlockInfo_.size_ + myConfig.PadBytes_ * 2 + ObjectSize);
	myConfig.InterAlignSize_ = myConfig.Alignment_ ? (interTotalSize % myConfig.Alignment_) : 0;
	interPageSectionSize = interTotalSize + myConfig.InterAlignSize_;
	// total alignment size
	size_t totalAlignmentSizeInPage = myConfig.LeftAlignSize_ + myConfig.InterAlignSize_ * (myConfig.ObjectsPerPage_ - 1);

	// Save total size
	myStats.PageSize_ = totalObjectSizeInPage + totalPaddingSizeInPage + totalHeaderSizeInPage + totalAlignmentSizeInPage + sizeof(void*);

	// Allocate the first page
	allocate_new_page();
}

ObjectAllocator::~ObjectAllocator()
{

	GenericObject* nextPage;

	while (PageList_) {
		// If we have an external header and its not freed before, we're deleting all of them
		if (myConfig.HBlockInfo_.type_ == OAConfig::HBLOCK_TYPE::hbExternal) {
			unsigned char* blockBegin = reinterpret_cast<unsigned char*>(PageList_);
			unsigned char* blockIterator = blockBegin + leftPageSectionSize;
			while (static_cast<unsigned int>(blockIterator - blockBegin) < myStats.PageSize_) {
				unsigned char* headerBlockIter = blockIterator - myConfig.PadBytes_ - myConfig.HBlockInfo_.size_;
				free_external_header(headerBlockIter);
				blockIterator += interPageSectionSize;
			}
		}
		nextPage = PageList_->Next;
		delete[](reinterpret_cast<unsigned char*>(PageList_));
		PageList_ = nextPage;
	}
}

void * ObjectAllocator::Allocate(const char * label)
{
	if (myConfig.UseCPPMemManager_)
	{
		try {
			void* allocatedObject = ::operator new(myStats.ObjectSize_);

			// Bookkeeping
			++myStats.Allocations_;
			++myStats.ObjectsInUse_;
			--myStats.FreeObjects_;
			if (myStats.ObjectsInUse_ > myStats.MostObjects_) {
				myStats.MostObjects_ = myStats.ObjectsInUse_;
			}

			return allocatedObject;
		}
		catch (std::bad_alloc& e) {
			throw OAException(OAException::E_NO_MEMORY, "Cannot allocate new object - no physical memory left: " + std::string(e.what()));
		}
	}

	// We everything is full, we need a new page
	if (!FreeList_) {
		allocate_new_page();
	}

	// Get the next free block
	GenericObject* objectToBeReturned = FreeList_;
	FreeList_ = FreeList_->Next;

	// Bookkeeping
	++myStats.Allocations_;
	++myStats.ObjectsInUse_;
	--myStats.FreeObjects_;
	if (myStats.ObjectsInUse_ > myStats.MostObjects_) {
		myStats.MostObjects_ = myStats.ObjectsInUse_;
	}

	unsigned char* headerBlockIter = reinterpret_cast<unsigned char*>(objectToBeReturned) - myConfig.PadBytes_ - myConfig.HBlockInfo_.size_;
	unsigned short counter;

	switch (myConfig.HBlockInfo_.type_)
	{
	case OAConfig::HBLOCK_TYPE::hbExtended:
		set_mem_and_move(&headerBlockIter, 0, myConfig.HBlockInfo_.additional_); //user defined
		memcpy(&counter, headerBlockIter, sizeof(counter));
		++counter;
		memcpy(headerBlockIter, &counter, sizeof(counter));
		headerBlockIter += sizeof(counter);
		//DumpPages();
	case OAConfig::HBLOCK_TYPE::hbBasic:
		memcpy(headerBlockIter, &myStats.Allocations_, sizeof(unsigned));
		headerBlockIter += sizeof(unsigned);
		set_mem_and_move(&headerBlockIter, 1, sizeof(char)); //toggles the last bit
		break;
	case OAConfig::HBLOCK_TYPE::hbExternal:
	{
		
		MemBlockInfo* memBlock = new MemBlockInfo();
		memBlock->in_use = true;
		if (label) {
			memBlock->label = new char[strlen(label) + 1];
			strcpy(memBlock->label, label);
		}
		else {
			memBlock->label = NULL;
		}
		
		memBlock->alloc_num = myStats.Allocations_;
		MemBlockInfo** memBlockP = &memBlock;
		memcpy(headerBlockIter, memBlockP, sizeof(void*));
		break;
	}
	case OAConfig::HBLOCK_TYPE::hbNone:
		break;
	default:
		break;
	}

	//DumpPages(32);

	// set patterns if debug is on
	if (myConfig.DebugOn_) {
		memset(reinterpret_cast<unsigned char*>(objectToBeReturned), ALLOCATED_PATTERN, myStats.ObjectSize_);
	}
	
	//DumpPages(32);

	return objectToBeReturned;
}

void ObjectAllocator::Free(void * Object)
{
	if (myConfig.UseCPPMemManager_)
	{
		::operator delete(Object);
	}
	else {
		unsigned char* memoryPointer = reinterpret_cast<unsigned char*>(Object);
		if (myConfig.DebugOn_) {

			// Check for double frees
			check_double_free(memoryPointer);
			// Check for corruption
			check_corruption(memoryPointer);
			// Check for Page boundaries
			check_boundary(memoryPointer);
			
			memset(memoryPointer, FREED_PATTERN, myStats.ObjectSize_);

		}

		unsigned char* headerBlockIter = memoryPointer - myConfig.PadBytes_ - myConfig.HBlockInfo_.size_;

		switch (myConfig.HBlockInfo_.type_)
		{
		case OAConfig::HBLOCK_TYPE::hbExtended:
			set_mem_and_move(&headerBlockIter, 0, myConfig.HBlockInfo_.additional_); //0 out user data
			headerBlockIter += sizeof(unsigned short); //move past the use counter -> we don't change it
		case OAConfig::HBLOCK_TYPE::hbBasic:
			set_mem_and_move(&headerBlockIter, 0, sizeof(unsigned int)); //reset alloc number
			set_mem_and_move(&headerBlockIter, 0, sizeof(char)); //toggle in-use
			break;
		case OAConfig::HBLOCK_TYPE::hbExternal:
		{
			free_external_header(headerBlockIter);
			memset(headerBlockIter, 0, myConfig.HBlockInfo_.size_);
			break;
		}
		case OAConfig::HBLOCK_TYPE::hbNone:
		default:
			break;
		}


		put_on_freelist(Object);

	}

	++myStats.Deallocations_;
	++myStats.FreeObjects_;
	--myStats.ObjectsInUse_;

}

unsigned ObjectAllocator::DumpMemoryInUse(DUMPCALLBACK fn) const
{
	unsigned counter = 0;
	GenericObject* pageListIterator = PageList_;
	unsigned char * pageIterator;
	unsigned char * pageBegin;
	while (pageListIterator) {
		pageBegin = reinterpret_cast<unsigned char*>(pageListIterator);
		pageIterator = pageBegin + leftPageSectionSize;

		while (static_cast<unsigned int>(pageIterator - pageBegin) < myStats.PageSize_) {
			if (!is_object_in_free_list(pageIterator)) {
				fn(pageIterator, myStats.ObjectSize_);
				++counter;
			}
			pageIterator += interPageSectionSize;
		}
		pageListIterator = pageListIterator->Next;
	}

	return counter;
}

unsigned ObjectAllocator::ValidatePages(VALIDATECALLBACK fn) const
{
	if (!myConfig.DebugOn_ || myConfig.PadBytes_ == 0)
		return 0;


	unsigned counter = 0;
	GenericObject* pageListIterator = PageList_;
	unsigned char * pageIterator;
	unsigned char * pageBegin;
	while (pageListIterator) {
		pageBegin = reinterpret_cast<unsigned char*>(pageListIterator);
		pageIterator = pageBegin + leftPageSectionSize;

		while (static_cast<unsigned int>(pageIterator - pageBegin) < myStats.PageSize_) {
			try {
				check_corruption(pageIterator);
			}
			catch (OAException & e) {
				//we don't need to check exception type
				//since this function only throws bad_corruption
				fn(pageIterator, myStats.ObjectSize_);
				++counter;
			}
			pageIterator += interPageSectionSize;
		}
		pageListIterator = pageListIterator->Next;
	}

	return counter;
}

unsigned ObjectAllocator::FreeEmptyPages(void)
{
	GenericObject* prevPage = NULL;
	GenericObject* currentPage = PageList_;
	unsigned char * pageIterator;
	unsigned char * pageBegin;
	unsigned int counter = 0;

	bool isPageEmpty = true;
	while (currentPage) {
		pageBegin = reinterpret_cast<unsigned char*>(currentPage);
		pageIterator = pageBegin + leftPageSectionSize;

		while (static_cast<unsigned int>(pageIterator - pageBegin) < myStats.PageSize_) {
			if (!is_object_in_free_list(pageIterator)) {
				isPageEmpty = false;
				break;
			}
			pageIterator += interPageSectionSize;
		}

		if (isPageEmpty) {
			GenericObject* pageToDelete = currentPage;
			if (currentPage == PageList_)
				PageList_ = currentPage->Next;
			currentPage = currentPage->Next;
			if(prevPage)
				prevPage->Next = currentPage;
			FreePage(pageToDelete);
			isPageEmpty = true;
			++counter;
		}
		else {
			prevPage = currentPage;
			currentPage = currentPage->Next;
		}


	}

	return counter;
}

bool ObjectAllocator::ImplementedExtraCredit(void)
{
	return false;
}

void ObjectAllocator::SetDebugState(bool State)
{
	myConfig.DebugOn_ = State;
}

const void * ObjectAllocator::GetFreeList(void) const
{
	return FreeList_;
}

const void * ObjectAllocator::GetPageList(void) const
{
	return PageList_;
}

OAConfig ObjectAllocator::GetConfig(void) const
{
	return myConfig;
}

OAStats ObjectAllocator::GetStats(void) const
{
	return myStats;
}

void ObjectAllocator::allocate_new_page(void)
{
	try {
		// Check if max. number of pages is reached
		if (myConfig.MaxPages_ != 0 && myStats.PagesInUse_ == myConfig.MaxPages_) {
			throw OAException(OAException::E_NO_PAGES, OUT_OF_LOGICAL_MEMORY_ERROR);
		}

		// Allocate memory and increment pages currently allocated
		unsigned char* newPage = new unsigned char[myStats.PageSize_];
		// Set everything to UNALLOCATED_PATTERN
		if(myConfig.DebugOn_)
			memset(newPage, UNALLOCATED_PATTERN, myStats.PageSize_);
		// Link pages
		GenericObject* oldPage = PageList_;
		PageList_ = reinterpret_cast<GenericObject*>(newPage);
		PageList_->Next = oldPage;

		//DumpPages(32);

		// Assign free list
		initialize_page(PageList_);

	}
	catch (std::bad_alloc & e) {
		throw OAException(OAException::E_NO_MEMORY, OUT_OF_PHYSICAL_MEMORY_ERROR);
	}
	// Bookkeeping
	++myStats.PagesInUse_;
	myStats.FreeObjects_ += myConfig.ObjectsPerPage_;
}

void ObjectAllocator::put_on_freelist(void * Object)
{
	GenericObject* nextObject = FreeList_;
	FreeList_ = reinterpret_cast<GenericObject*>(Object);
	FreeList_->Next = nextObject;
}

void ObjectAllocator::initialize_page(GenericObject* pageListBegin)
{
	// Leftmost block
	unsigned char* pageBegin = reinterpret_cast<unsigned char*>(pageListBegin);
	unsigned char* pageIterator = pageBegin; // move iterator after next block
	if (myConfig.DebugOn_) {
		pageIterator += sizeof(void*); // Pass through the first next pointer
		// Left alignment, header and padding
		set_non_data_block_pattern(&pageIterator, myConfig.LeftAlignSize_);
		// At this point, page Iterator is pointing to the beginning of next block
		move_freelist(pageIterator);
	}
	else {
		move_freelist(pageIterator += leftPageSectionSize);
	}
	

	// Inter blocks
	// Fill the page using the free list

	// I know that two while looks disgusting but this way I can only do one if check for the entire loop
	if (myConfig.DebugOn_) {
		while (static_cast<unsigned int>((pageIterator + interPageSectionSize) - pageBegin) < myStats.PageSize_) {
			// Inter first padding - moves the pointer to the end of data first
			set_mem_and_move(&(pageIterator += myStats.ObjectSize_), PAD_PATTERN, myConfig.PadBytes_);
			// Inter alignment, header and padding
			set_non_data_block_pattern(&pageIterator, myConfig.InterAlignSize_);
			// At this point, page Iterator is pointing to the beginning of next block
			move_freelist(pageIterator);
		}
	}
	else {
		while (static_cast<unsigned int>((pageIterator + interPageSectionSize) - pageBegin) < myStats.PageSize_) {
			move_freelist(pageIterator += interPageSectionSize);
		}
	}
	
	// Also set the last padding block
	if (myConfig.DebugOn_)
		set_mem_and_move(&(pageIterator += myStats.ObjectSize_), PAD_PATTERN, myConfig.PadBytes_);

	//DumpPages(32);

}

void ObjectAllocator::set_mem_and_move(unsigned char ** begin, int value, size_t size)
{
	memset(*begin, value, size);
	*begin += size;
}

void ObjectAllocator::set_non_data_block_pattern(unsigned char ** begin, size_t alignSize)
{
	// Left alignment
	set_mem_and_move(begin, ALIGN_PATTERN, alignSize);
	// Left header
	set_mem_and_move(begin, 0, myConfig.HBlockInfo_.size_);
	// Left padding
	set_mem_and_move(begin, PAD_PATTERN, myConfig.PadBytes_);

	//DumpPages(32);
}

void ObjectAllocator::move_freelist(unsigned char* position)
{
	GenericObject* PrevBlock = FreeList_;
	FreeList_ = reinterpret_cast<GenericObject*>(position);
	FreeList_->Next = PrevBlock;
}

bool ObjectAllocator::is_object_in_free_list(void * Object) const
{
	// Check through header first
	if (myConfig.HBlockInfo_.type_ == OAConfig::HBLOCK_TYPE::hbBasic
		|| myConfig.HBlockInfo_.type_ == OAConfig::HBLOCK_TYPE::hbExtended) {
		unsigned char* blockIter = reinterpret_cast<unsigned char*>(Object) - myConfig.PadBytes_ - sizeof(char);
		return *blockIter == 0;

	}
	else if (myConfig.HBlockInfo_.type_ == OAConfig::HBLOCK_TYPE::hbExternal) {
		unsigned char* blockIter = reinterpret_cast<unsigned char*>(Object) - myConfig.PadBytes_ - myConfig.HBlockInfo_.size_;
		MemBlockInfo** blockInfoP = reinterpret_cast<MemBlockInfo**>(blockIter);
		MemBlockInfo* blockInfo = *blockInfoP;
		if(blockInfo)
			return blockInfo->in_use;
	}

	GenericObject* currentObjectInFreeList = FreeList_;
	while (currentObjectInFreeList) {
		if (currentObjectInFreeList == Object)
			return true;
		currentObjectInFreeList = currentObjectInFreeList->Next;
	}
	return false;
}

void ObjectAllocator::free_external_header(unsigned char * object)
{
	MemBlockInfo** blockInfoP = reinterpret_cast<MemBlockInfo**>(object);
	MemBlockInfo* blockInfo = *blockInfoP;
	if (blockInfo && blockInfo->label) {
		delete[](blockInfo->label);
	}

	delete blockInfo;
}

void ObjectAllocator::check_boundary(unsigned char * Object) const
{
	// TODO: Try to find a solution without going through page headers
	GenericObject* currentPage = PageList_;
	unsigned char* currentPageBegin;
	// Find the page this memory belongs to
	while (currentPage) { // Check all pages and see if this object is in between my pages
		currentPageBegin = reinterpret_cast<unsigned char*>(currentPage);
		if (Object > currentPageBegin && Object < (currentPageBegin + myStats.PageSize_)) {
			break;
		}
		currentPage = currentPage->Next;
	}
	// After loop execution:
	// if (currentPage) currentPage = the page mem block is in and currentPageBegin = char* of it
	if (currentPage) {
		unsigned char* firstBlock = currentPageBegin + leftPageSectionSize;
		size_t blockDistance = Object - firstBlock;
		if (blockDistance % interPageSectionSize != 0) { // If total distance cannot be divided into block sizes
			throw OAException(OAException::E_BAD_BOUNDARY, "Object given is not in correct boundary");
		}
	}
	else {
		throw OAException(OAException::E_BAD_ADDRESS, "Object given is not registered in any of the pages");
	}

}

void ObjectAllocator::check_double_free(unsigned char * Object) const
{
	// Check for double free
	// This code might seem like a hack. That's because it is (sort of)
	// Explanation: Basically, if we've freed this object before the first sizeof(void*) bytes will be
	// containing the "Next" pointer for the free list
	// But the rest of this mem block will memset to "FREED_PATTERN"
	// Will not work if the sizeof(object) == sizeof(void*) (obviously) - Hence the size check
	if (myStats.ObjectSize_ > sizeof(void*)) {
		if (*(Object + sizeof(void*)) == FREED_PATTERN) {
			throw OAException(OAException::E_MULTIPLE_FREE, "Object has been freed before: Multiple free");
		}
	}
	else if (is_object_in_free_list(Object)) { // Go through the free list to see if this is free TODO: check if there is a O(1) solution
		throw OAException(OAException::E_MULTIPLE_FREE, "Object has been freed before: Multiple free");
	}
}

void ObjectAllocator::check_corruption(unsigned char * Object) const
{
	if (myConfig.PadBytes_ == 0)
		return;

	unsigned char* objectBegin = reinterpret_cast<unsigned char*>(Object);
	unsigned char* objectEnd = objectBegin + myStats.ObjectSize_;

	// Get a pointer to the head padding block
	unsigned char* paddingIterator = objectBegin - myConfig.PadBytes_;
	// Iterate through all padding bytes to see if they're intact
	while (paddingIterator != objectBegin) {
		if (*(paddingIterator++) != PAD_PATTERN) {
			throw OAException(OAException::E_CORRUPTED_BLOCK, "Head padding for this block doesn't match the pattern.");
		}
	}

	// Point to the end of tail padding block
	paddingIterator = objectEnd + myConfig.PadBytes_ - 1;
	while (paddingIterator != objectEnd) {
		if(*(paddingIterator--) != PAD_PATTERN)
			throw OAException(OAException::E_CORRUPTED_BLOCK, "Tail padding for this block doesn't match the pattern.");
	}
}

void ObjectAllocator::PrintConfig()
{
	cout << "Object size = " << myStats.ObjectSize_;
	cout << ", Page size = " << myStats.PageSize_;
	cout << ", Pad bytes = " << myConfig.PadBytes_;
	cout << ", ObjectsPerPage = " << myConfig.ObjectsPerPage_;
	cout << ", MaxPages = " << myConfig.MaxPages_;
	cout << ", MaxObjects = " << myConfig.ObjectsPerPage_ * myConfig.MaxPages_;
	cout << endl;
	cout << "Alignment = " << myConfig.Alignment_;
	cout << ", LeftAlign = " << myConfig.LeftAlignSize_;
	cout << ", InterAlign = " << myConfig.InterAlignSize_;
	cout << ", HeaderBlocks = ";
	if (myConfig.HBlockInfo_.type_ == OAConfig::hbNone)
		cout << "None";
	else if (myConfig.HBlockInfo_.type_ == OAConfig::hbBasic)
		cout << "Basic";
	else if (myConfig.HBlockInfo_.type_ == OAConfig::hbExtended)
		cout << "Extended";
	else if (myConfig.HBlockInfo_.type_ == OAConfig::hbExternal)
		cout << "External";
	cout << ", Header size = " << myConfig.HBlockInfo_.size_;
	cout << endl;
}

void ObjectAllocator::DumpPages(unsigned width)
{
	const unsigned char *pages = reinterpret_cast<const unsigned char *>(PageList_);
	const unsigned char *realpage = pages;

	size_t header_size = myConfig.HBlockInfo_.size_;

	while (pages)
	{
		unsigned count = 0;

		printf("XXXXXXXX\n");

		// print column header
		for (unsigned j = 0; j < width; j++)
			printf(" %2i", j);
		printf("\n");

		// "Next page" pointer in the page
		for (unsigned j = 0; j < sizeof(void *); pages++, count++, j++)
			printf(" %s", "XX");



		// Left leading alignment bytes
		if (myConfig.Alignment_ > 1)
		{
			// leading alignment block (if any)
			for (unsigned j = 0; j < myConfig.LeftAlignSize_; count++, j++)
			{
				if (count >= width)
				{
					printf("\n");
					count = 0;
				}
				printf(" %02X", *pages++);
			}
		}

		// Dump each object and its associated info
		for (unsigned int i = 0; i < myConfig.ObjectsPerPage_; i++)
		{
			// inter-block alignment (not on first block)
			if (i > 0)
			{
				for (unsigned j = 0; j < myConfig.InterAlignSize_; count++, j++)
				{
					if (count >= width)
					{
						printf("\n");
						count = 0;
					}
					printf(" %02X", *pages++);
				}
			}

			// header block bytes
			for (unsigned j = 0; j < header_size; count++, j++)
			{
				if (count >= width)
				{
					printf("\n");
					count = 0;
				}
				printf(" %02X", *pages++);
			}

			// left padding
			for (unsigned j = 0; j < myConfig.PadBytes_; count++, j++)
			{
				if (count >= width)
				{
					printf("\n");
					count = 0;
				}
				printf(" %02X", *pages++);
			}

			// possible next pointer (zero it out)
			for (unsigned j = 0; j < sizeof(void *); count++, pages++, j++)
			{
				if (count >= width)
				{
					printf("\n");
					count = 0;
				}

				printf(" %s", "XX");
			}

			// remaining bytes
			for (unsigned j = 0; j < myStats.ObjectSize_ - sizeof(void *); count++, j++)
			{
				if (count >= width)
				{
					printf("\n");
					count = 0;
				}
				printf(" %02X", *pages++);
			}

			// right pad bytes
			for (unsigned j = 0; j < myConfig.PadBytes_; count++, j++)
			{
				if (count >= width)
				{
					printf("\n");
					count = 0;
				}
				printf(" %02X", *pages++);
			}

		}
		printf("\n\n");

		pages = reinterpret_cast<const unsigned char *>((reinterpret_cast<const GenericObject *>(realpage))->Next);
		realpage = pages;
	}
}

void ObjectAllocator::FreePage(GenericObject * pageHead)
{
	unsigned char * pageBegin = reinterpret_cast<unsigned char*>(pageHead);
	unsigned char * pageIterator;
	GenericObject* currentBlock = FreeList_;
	GenericObject* prevBlock = NULL;
	GenericObject* blockToDelete;
	bool isDeleted = false;
	while (currentBlock) {
		pageIterator = pageBegin + leftPageSectionSize;
		while (static_cast<unsigned int>(pageIterator - pageBegin) < myStats.PageSize_) {
			blockToDelete = reinterpret_cast<GenericObject*>(pageIterator);
			if (blockToDelete == currentBlock) {
				if(myConfig.HBlockInfo_.type_ == OAConfig::HBLOCK_TYPE::hbExternal)
					free_external_header(pageIterator);
				isDeleted = true;
				break;
			}

			pageIterator += interPageSectionSize;
		}

		if (isDeleted) {
			if (currentBlock == FreeList_)
				FreeList_ = currentBlock->Next;
			currentBlock = currentBlock->Next;
			if(prevBlock)
				prevBlock->Next = currentBlock;
			isDeleted = false;
		}
		else {
			prevBlock = currentBlock;
			currentBlock = currentBlock->Next;
		}

	}

	delete[](reinterpret_cast<unsigned char*>(pageHead));

}
