#include "ObjectAllocator.h"


ObjectAllocator::ObjectAllocator(size_t ObjectSize, const OAConfig & config) : PageList_(NULL), FreeList_(NULL), myConfig(config)
{
	// Save each object's size
	myStats.ObjectSize_ = ObjectSize;

	// Calculate a page's total size
	// Total Object Size
	size_t totalObjectSizeInPage = myConfig.ObjectsPerPage_ * ObjectSize;
	// Total Padding Size
	size_t totalPaddingSizeInPage = myConfig.ObjectsPerPage_ * myConfig.PadBytes_ * 2; // Padding to the left and right
	// HeaderSize
	size_t totalHeaderSizeInPage = myConfig.HBlockInfo_.size_ * myConfig.ObjectsPerPage_; // One header per object
	// Alignment
	// For left alignment: One header, One pad-byte and one "Next" pointer
	myConfig.LeftAlignSize_ = (myConfig.HBlockInfo_.size_ + myConfig.PadBytes_ + sizeof(void*)) % myConfig.Alignment_;
	// For inter alignment = One header, two pad-bytes (after object and before next object) and object itself
	myConfig.InterAlignSize_ = (myConfig.HBlockInfo_.size_ + myConfig.PadBytes_ * 2 + ObjectSize) % myConfig.Alignment_;
	size_t totalAlignmentSizeInPage = myConfig.LeftAlignSize_ + myConfig.InterAlignSize_ * (myConfig.ObjectsPerPage_ - 1);
	
	// Save total size
	myStats.PageSize_ = totalObjectSizeInPage + totalPaddingSizeInPage + totalHeaderSizeInPage + totalAlignmentSizeInPage;

}

ObjectAllocator::~ObjectAllocator()
{
}

void * ObjectAllocator::Allocate(const char * label)
{
	return nullptr;
}

void ObjectAllocator::Free(void * Object)
{
}

unsigned ObjectAllocator::DumpMemoryInUse(DUMPCALLBACK fn) const
{
	return 0;
}

unsigned ObjectAllocator::ValidatePages(VALIDATECALLBACK fn) const
{
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
}

const void * ObjectAllocator::GetFreeList(void) const
{
	return nullptr;
}

const void * ObjectAllocator::GetPageList(void) const
{
	return nullptr;
}

OAConfig ObjectAllocator::GetConfig(void) const
{
	return OAConfig();
}

OAStats ObjectAllocator::GetStats(void) const
{
	return OAStats();
}

void ObjectAllocator::allocate_new_page(void)
{
	try {
		// Check if max. number of pages is reached
		if (myStats.PagesInUse_ == myConfig.MaxPages_) {
			throw OAException(OAException::E_NO_PAGES, "Cannot allocate new page - out of pages");
		}

		// Allocate memory and increment pages currently allocated
		void* newPage = ::operator new(myStats.PageSize_);
		++myStats.PagesInUse_;
		// Link pages
		GenericObject* oldPage = PageList_;
		PageList_ = reinterpret_cast<GenericObject*>(newPage);
		PageList_->Next = oldPage;



	}
	catch (std::bad_alloc & e) {
		throw OAException(OAException::E_NO_MEMORY, "Cannot allocate new page - out of physical memory: " + std::string(e.what()));
	}
}

void ObjectAllocator::put_on_freelist(void * Object)
{
}

ObjectAllocator & ObjectAllocator::operator=(const ObjectAllocator & oa)
{
	// It should never come here - this is private
	return *this;

}
