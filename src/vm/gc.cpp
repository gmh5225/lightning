#include <vm/gc.hpp>
#include <vm/state.hpp>
#include <vm/table.hpp>
#include <vm/string.hpp>
#include <span>


namespace li::gc {
	void header::gc_init(page* p, vm* L, uint32_t clen, value_type t) {
		gc_type        = t;
		num_chunks     = clen;
		page_offset    = (uintptr_t(this) - uintptr_t(p)) >> 12;
		stage          = L ? L->stage : 0;
	}
	bool header::gc_tick(stage_context s, bool weak) {
		LI_ASSERT(!is_free());

		// If already iterated, skip.
		//
		if (stage == s) [[likely]] {
			return true;
		}

		// Update stage, recurse.
		//
		stage = s;
		if (gc_type <= type_gc_last_traversable) {
			if (gc_type == type_array)
				traverse(s, (array*) this);
			else if (gc_type == type_table)
				traverse(s, (table*) this);
			else if (gc_type == type_function)
				traverse(s, (function*) this);
		}

		// Increment counter.
		//
		get_page()->alive_objects++;
		return true;
	}

	
	// Allocates an uninitialized chunk.
	//
	std::pair<page*, void*> state::allocate_uninit(vm* L, uint32_t clen) {
		LI_ASSERT(clen != 0);

		// Try allocating from a free list.
		//
		auto try_alloc_class = [&](size_t scl, bool excess) LI_INLINE -> std::pair<page*, void*> {
			header* it   = free_lists[scl];
			header* prev = nullptr;
			while (it) {
				if (excess || it->num_chunks >= clen) {
					// Unlink the entry.
					//
					if (prev) {
						prev->set_next_free(it->get_next_free());
					} else {
						free_lists[scl] = it->get_next_free();
					}

					// Get the page and change the type.
					//
					auto* page  = it->get_page();
					it->gc_type = type_gc_uninit;
					page->num_objects++;

					// If we didn't allocate all of the space, re-insert into the free-list.
					//
					if (uint32_t leftover = it->num_chunks - clen) {
						it->num_chunks   = clen;
						auto& free_list  = free_lists[size_class_of(leftover)];
						auto* fh2        = it->next();
						fh2->gc_type     = type_gc_free;
						fh2->num_chunks  = leftover;
						fh2->page_offset = (uintptr_t(fh2) - uintptr_t(page)) >> 12;
						fh2->set_next_free(free_list);
						free_list = fh2;
					}

					// Return the result.
					//
					return {page, it};
				} else {
					prev = it;
					it   = it->get_next_free();
				}
			}
			return {nullptr, nullptr};
		};
		{
			size_t size_class = size_class_of(clen);
			auto   alloc      = try_alloc_class(size_class, false);
			if (!alloc.second && ++size_class != std::size(size_classes)) {
				alloc = try_alloc_class(size_class, true);
			}
			if (alloc.second)
				return alloc;
		}

		// Find a page with enough size to fit our object or allocate one.
		//
		auto* pg = for_each([&](page* p) { return p->check_space(clen); });
		if (!pg) {
			pg = add_page(L, size_t(clen) << chunk_shift, false);
			if (!pg)
				return {nullptr, nullptr};
		}

		// Increment GC depth, return the result.
		//
		debt += clen;
		return {pg, pg->alloc_arena(clen)};
	}
	void state::free(vm* L, header* o, bool within_gc) {
		LI_ASSERT_MSG("Double free", !o->is_free());

		// Decrement counters.
		//
		auto* page = o->get_page();
		if (!within_gc)
			page->alive_objects--;
		page->num_objects--;

		// Run destructor if relevant.
		//
		if (o->gc_type == type_table) {
			((traitful_node<>*) o)->gc_destroy(L);
		}
#if LI_DEBUG
		memset(o + 1, 0xCC, o->object_bytes());
#endif

		// Insert into the free list or adjust arena.
		//
		if (o->next() != page->end()) {
			auto& free_list = free_lists[size_class_of(o->num_chunks)];
			o->gc_type      = type_gc_free;
			o->set_next_free(free_list);
			free_list = o;
		} else {
			page->next_chunk -= o->num_chunks;
		}
	}

	static void traverse_live(vm* L, stage_context s) {
		// Stack.
		//
		for (auto& e : std::span{L->stack, (size_t) L->stack_top}) {
			if (e.is_gc())
				e.as_gc()->gc_tick(s);
		}
		std::prev(((header*) L->stack))->gc_tick(s);

		// Globals.
		//
		L->globals->gc_tick(s);

		// Strings.
		//
		((header*) L->empty_string)->gc_tick(s);
		((header*) L->strset)->gc_tick(s);
	}

	LI_COLD void state::collect(vm* L) {
		// Reset GC tick.
		//
		ticks = gc_interval;
		debt  = 0;

		// Clear alive counter in all pages.
		//
		initial_page->alive_objects = 1; // vm
		for (auto it = initial_page->next; it != initial_page; it = it->next) {
			it->alive_objects = 0;
		}

		// Mark all alive objects.
		//
		L->stage ^= 1;
		stage_context ms{bool(L->stage)};
		traverse_live(L, ms);

		// Free all dead objects.
		//
		page* dead_page_list = nullptr;
		for_each([&](page* it) {
			if (it->alive_objects != it->num_objects) {
				it->for_each([&](header* obj) {
					if (!obj->is_free() && obj->stage != ms) {
						free(L, obj, true);
					}
					return false;
				});
				if (!it->alive_objects && !greedy) {
					util::unlink(it);
					if (!dead_page_list) {
						it->next       = nullptr;
						dead_page_list = it;
					} else {
						it->next       = dead_page_list;
						dead_page_list = it;
					}
				}
			}
			return false;
		});

		// Sweep dead references.
		//
		strset_sweep(L, ms);

		// If we can free any pages, do so.
		//
		if (dead_page_list) [[unlikely]] {
			// Fix free lists.
			//
			for (size_t i = 0; i != free_lists.size(); i++) {
				header** prev = &free_lists[i];
				for (auto it = *prev; it;) {
					auto* next = it->get_next_free();
					if (!it->get_page()->alive_objects) {
						*prev = it->get_next_free();
					} else {
						prev = &it->ref_next_free();
					}
					it = next;
				}
			}

			// Deallocate.
			//
			while (dead_page_list) {
				auto i = std::exchange(dead_page_list, dead_page_list->next);
				alloc_fn(alloc_ctx, i, i->num_pages, false);
			}
		}
	}
};