#include "ObjectAllocator.h"

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
		nextPage = PageList_->Next;
		delete[](reinterpret_cast<unsigned char*>(PageList_));
		PageList_ = nextPage;
	}
}

void * ObjectAllocator::Allocate(const char * label)
{
	if (label) {
		printf("%s", label);
	}

	if (myConfig.UseCPPMemManager_)
	{
		++myStats.Allocations_;
		return ::operator new(myStats.ObjectSize_);
	}

	// We everything is full, we need a new page
	if (!FreeList_) {
		allocate_new_page();
	}

	GenericObject* objectToBeReturned = FreeList_;
	FreeList_ = FreeList_->Next;
	memset(reinterpret_cast<unsigned char*>(objectToBeReturned), ALLOCATED_PATTERN, myStats.ObjectSize_);
	// Bookkeeping
	++myStats.Allocations_;
	++myStats.ObjectsInUse_;
	--myStats.FreeObjects_;
	if (myStats.ObjectsInUse_ > myStats.MostObjects_) {
		myStats.MostObjects_ = myStats.ObjectsInUse_;
	}

	return objectToBeReturned;
}

void ObjectAllocator::Free(void * Object)
{
	if (myConfig.UseCPPMemManager_)
	{
		++myStats.Deallocations_;
		::operator delete(Object);
	}
	else {
		if (myConfig.DebugOn_) {
			unsigned char* memoryPointer = reinterpret_cast<unsigned char*>(Object);

			// Check for double frees
			check_double_free(memoryPointer);
			// Check for corruption
			check_corruption(memoryPointer);
			// Check for Page boundaries
			check_boundary(memoryPointer);
			
			memset(memoryPointer, FREED_PATTERN, myStats.ObjectSize_);
		}
		put_on_freelist(Object);

	}

	++myStats.Deallocations_;
	++myStats.FreeObjects_;
	--myStats.ObjectsInUse_;

}

unsigned ObjectAllocator::DumpMemoryInUse(DUMPCALLBACK fn) const
{
	fn(PageList_, 10);
	return 0;
}

unsigned ObjectAllocator::ValidatePages(VALIDATECALLBACK fn) const
{
	fn(PageList_, 10);
	return 0;
}

unsigned ObjectAllocator::FreeEmptyPages(void)
{
	return 0;
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
}

void ObjectAllocator::move_freelist(unsigned char* position)
{
	GenericObject* PrevBlock = FreeList_;
	FreeList_ = reinterpret_cast<GenericObject*>(position);
	FreeList_->Next = PrevBlock;
}

bool ObjectAllocator::is_object_in_free_list(void * Object)
{
	GenericObject* currentObjectInFreeList = FreeList_;
	while (currentObjectInFreeList) {
		if (currentObjectInFreeList == Object)
			return true;
		currentObjectInFreeList = currentObjectInFreeList->Next;
	}
	return false;
}

void ObjectAllocator::check_boundary(unsigned char * Object)
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

void ObjectAllocator::check_double_free(unsigned char * Object)
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

void ObjectAllocator::check_corruption(unsigned char * Object)
{
	if (Object)
		return;
}

