#include "memory.h"
#include "dlmalloc.h"

#include <stdlib.h>
#include <assert.h>
#include <new>

namespace {
	using namespace foundation;

	// Header stored at the beginning of a memory allocation to indicate the
	// size of the allocated data.
	struct Header {
		uint32_t size;
	};

	// If we need to align the memory allocation we pad the header with this
	// value after storing the size. That way we can 
	const uint32_t HEADER_PAD_VALUE = 0xffffffffu;

	// Given a pointer to the header, returns a pointer to the data that follows it.
	inline void *data_pointer(Header *header, uint32_t align) {
		void *p = header + 1;
		return memory::align_forward(p, align);
	}

	// Given a pointer to the data, returns a pointer to the header before it.
	inline Header *header(void *data)
	{
		uint32_t *p = (uint32_t *)data;
		while (p[-1] == HEADER_PAD_VALUE)
			--p;
		return (Header *)p - 1;
	}

	// Stores the size in the header and pads with HEADER_PAD_VALUE up to the
	// data pointer.
	inline void fill(Header *header, void *data, uint32_t size)
	{
		header->size = size;
		uint32_t *p = (uint32_t *)(header + 1);
		while (p < data)
			*p++ = HEADER_PAD_VALUE;
	}

	/// An allocator used to allocate temporary "scratch" memory. The allocator
	/// uses a fixed size ring buffer to services the requests.
	///
	/// Memory is always always allocated linearly. An allocation pointer is
	/// advanced through the buffer as memory is allocated and wraps around at
	/// the end of the buffer. Similarly, a free pointer is advanced as memory
	/// is freed.
	///
	/// It is important that the scratch allocator is only used for short-lived
	/// memory allocations. A long lived allocator will lock the "free" pointer
	/// and prevent the "allocate" pointer from proceeding past it, which means
	/// the ring buffer can't be used.
	/// 
	/// If the ring buffer is exhausted, the scratch allocator will use its backing
	/// allocator to allocate memory instead.
	class ScratchAllocator : public Allocator
	{
		Allocator &_backing;
		
		// Start and end of the ring buffer.
		char *_begin, *_end;

		// Pointers to where to allocate memory and where to free memory.
		char *_allocate, *_free;
		
	public:
		/// Creates a ScratchAllocator. The allocator will use the backing
		/// allocator to create the ring buffer and to service any requests
		/// that don't fit in the ring buffer.
		///
		/// size specifies the size of the ring buffer.
		ScratchAllocator(Allocator &backing, uint32_t size) : _backing(backing) {
			_begin = (char *)_backing.allocate(size);
			_end = _begin + size;
			_allocate = _begin;
			_free = _begin;
		}

		~ScratchAllocator() {
			assert(_free == _allocate);
			_backing.deallocate(_begin);
		}

		bool in_use(void *p)
		{
			if (_free == _allocate)
				return false;
			if (_allocate > _free)
				return p >= _free && p < _allocate;
			return p >= _free || p < _allocate;
		}

		virtual void *allocate(uint32_t size, uint32_t align) {
			assert(align % 4 == 0);
			size = ((size + 3)/4)*4;

			char *p = _allocate;
			Header *h = (Header *)p;
			char *data = (char *)data_pointer(h, align);
			p = data + size;

			// Reached the end of the buffer, wrap around to the beginning.
			if (p > _end) {
				h->size = (_end - (char *)h) | 0x80000000u;
				
				p = _begin;
				h = (Header *)p;
				data = (char *)data_pointer(h, align);
				p = data + size;
			}
			
			// If the buffer is exhausted use the backing allocator instead.
			if (in_use(p))
				return _backing.allocate(size, align);

			fill(h, data, p - (char *)h);
			_allocate = p;
			return data;
		}

		virtual void deallocate(void *p) {
			if (!p)
				return;

			if (p < _begin || p >= _end) {
				_backing.deallocate(p);
				return;
			}

			// Mark this slot as free
			Header *h = header(p);
			assert((h->size & 0x80000000u) == 0);
			h->size = h->size | 0x80000000u;

			// Advance the free pointer past all free slots.
			while (_free != _allocate) {
				Header *h = (Header *)_free;
				if ((h->size & 0x80000000u) == 0)
					break;

				_free += h->size & 0x7fffffffu;
				if (_free == _end)
					_free = _begin;
			}
		}

		virtual uint32_t allocated_size(void *p) {
			Header *h = header(p);
			return h->size - ((char *)p - (char *)h);
		}

		virtual uint32_t total_allocated() {
			return _end - _begin;
		}
	};

	struct MemoryGlobals {
		//static const int ALLOCATOR_MEMORY = sizeof(HeapAllocator) + sizeof(ScratchAllocator);
		static const int ALLOCATOR_MEMORY = 1024;
		char buffer[ALLOCATOR_MEMORY];

		HeapAllocator *static_heap;
		PageAllocator *default_page_allocator;
		HeapAllocator *default_allocator;
		ScratchAllocator *default_scratch_allocator;

		MemoryGlobals() : static_heap(0), default_page_allocator(0), default_allocator(0), default_scratch_allocator(0) {}
	};

	MemoryGlobals _memory_globals;
}

namespace foundation
{

	///
	/// Heap Allocator
	///

	HeapAllocator::HeapAllocator(void *buffer, uint32_t size) 
			: _total_allocated(0) {
		_memory_space = create_mspace_with_base(nullptr, buffer, size, false);
		assert(_memory_space);
	}

	HeapAllocator::HeapAllocator(Allocator &backing_allocator)
			: _total_allocated(0) {
		_memory_space = create_mspace(&backing_allocator, 0, false);
		assert(_memory_space);
	}

	HeapAllocator::~HeapAllocator() {
		destroy_mspace(_memory_space);
		assert(_total_allocated == 0);
	}

 	void *HeapAllocator::allocate(uint32_t size, uint32_t align) {
 		void *p = mspace_memalign(_memory_space, align, size);
 		_total_allocated += allocated_size(p);
 		return p;
 	}

 	void HeapAllocator::deallocate(void *p) {
 		_total_allocated -= allocated_size(p);
 		mspace_free(_memory_space, p);
 	}

 	uint32_t HeapAllocator::allocated_size(void *p) {
 		return dlallocated_size(p);
 	}

	uint32_t HeapAllocator::total_allocated() {
		return _total_allocated;
	}

	///
	/// Page Allocator
	///

	// TODO: TR This is currently windows specific, add linux support
	PageAllocator::PageAllocator() 
			: _total_allocated(0) {
	}

	PageAllocator::~PageAllocator() {
		assert(_total_allocated == 0);
	}

 	void *PageAllocator::allocate(uint32_t size, uint32_t align) {
 		void *ptr = VirtualAlloc(0, size, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
 		auto rs = allocated_size(ptr);
 		_total_allocated += rs;
 		return ptr;
 	}

 	void PageAllocator::deallocate(void *p) {
 		_total_allocated -= allocated_size(p);
 		VirtualFree(p, 0, MEM_RELEASE);
 	}

 	uint32_t PageAllocator::allocated_size(void *p) {
 		MEMORY_BASIC_INFORMATION minfo;
 		VirtualQuery(p, &minfo, sizeof(minfo));
 		return minfo.RegionSize;
 	}

	uint32_t PageAllocator::total_allocated() {
		return _total_allocated;
	}

	///
	/// Memory globals
	///

	namespace memory_globals
	{
		void init(uint32_t temporary_memory) {
			char *buffer = _memory_globals.buffer;

			auto static_heap = new (buffer) HeapAllocator(buffer + sizeof(HeapAllocator), _memory_globals.ALLOCATOR_MEMORY - sizeof(HeapAllocator));
			_memory_globals.static_heap = static_heap;

			_memory_globals.default_page_allocator = static_heap->make_new<PageAllocator>();
			_memory_globals.default_allocator = static_heap->make_new<HeapAllocator>(*_memory_globals.default_page_allocator);
			_memory_globals.default_scratch_allocator = static_heap->make_new<ScratchAllocator>(*_memory_globals.default_allocator, temporary_memory);
		}

		Allocator &default_allocator() {
			return *_memory_globals.default_allocator;
		}

		Allocator &default_page_allocator() {
			return *_memory_globals.default_page_allocator;
		}

		Allocator &default_scratch_allocator() {
			return *_memory_globals.default_scratch_allocator;
		}

		void shutdown() {
			_memory_globals.static_heap->make_delete(_memory_globals.default_scratch_allocator);
			_memory_globals.static_heap->make_delete(_memory_globals.default_allocator);
			_memory_globals.static_heap->make_delete(_memory_globals.default_page_allocator);
			_memory_globals.static_heap->~HeapAllocator();

			_memory_globals = MemoryGlobals();
		}
	}
}
