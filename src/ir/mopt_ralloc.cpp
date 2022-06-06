#include <ir/opt.hpp>

/*

A) instruction traits [encoding, etc]
B) reg alloc


-> type & trait inference
---> register allocation
1) flags register
2) can't spill or re-type conditionally
- optimize type inference with dominator trees


-> register allocation

*/

/*
* TODO: Minimize lifetime by moving around
* %v8 = shr %v8 0x2f
  %v8 = sub %v8 0x9
	%v9 = movi 0xfffaffffffffffff <-- blocks like this.
	%v1 = movi %v9
  %f3 = cmp %v8 0x1
*/

static constexpr float RA_PRIO_HOT_BIAS = 12.0f;

namespace li::ir::opt {
	struct graph_node {
		util::bitset vtx                 = {};
		float        priority            = 0;
		intptr_t     coalescing_hints[4] = {0};  // offset
		uint8_t      hint_id             = 0;
		uint8_t      color               = {};
		bool         is_fp               = false;
		int32_t      spill_slot          = 0;

		void add_hint(graph_node* g) {
			coalescing_hints[hint_id++ % std::size(coalescing_hints)] = g - this;
		}
	};

	// Returns a view that can be enumerated as a series of mregs for a given bitset.
	//
	static auto regs_in(const util::bitset& bs) {
		return view::iota(0ull, bs.size()) | view::filter([&](size_t n) { return bs[n]; }) | view::transform([&](size_t n) { return mreg::from_uid(uint32_t(n)); });
	}

	// Returns true if the register does not require allocation.
	//
	static bool is_pseudo(mreg r) {
		return r.is_flag() || (r.is_virt() && r.virt() > 0 && r.virt() < vreg_first);
	}

	// Returns true if register should be included in the interference graph.
	//
	static bool interferes_with(mreg a, mreg b) {
		// Ignore pseudo registers.
		//
		if (is_pseudo(a) || is_pseudo(b))
			return false;

		// Match target class.
		//
		if (a.is_fp() != b.is_fp())
			return false;
		return true;
	}

	// Debug helpers.
	//
	static void print_graph(std::span<graph_node> gr) {
		printf("graph {\n node [colorscheme=set312 penwidth=5]\n");

		for (size_t i = 0; i != gr.size(); i++) {
			auto v = mreg::from_uid(i);
			if (gr[i].vtx.popcount() != 1)
				printf("r%u [color=%u label=\"%s\"];\n", v.uid(), gr[i].color, v.to_string().c_str());
		}

		for (size_t i = 0; i != gr.size(); i++) {
			for (size_t j = 0; j != gr.size(); j++) {
				if (i <= j && gr[i].vtx.get(j)) {
					printf("r%u -- r%u;\n", i, j);
				}
			}
		}
		printf("}\n");
	}
	static void print_lifetime(mprocedure* proc, std::span<graph_node> gr = {}) {
		puts("\n");
		for (auto& b : proc->basic_blocks) {
			printf("-- Block $%u", b.uid);
			if (b.hot < 0)
				printf(LI_CYN " [COLD %u]" LI_DEF, (uint32_t) -b.hot);
			if (b.hot > 0)
				printf(LI_RED " [HOT  %u]" LI_DEF, (uint32_t) b.hot);
			putchar('\n');

			printf("Out-Live = ");
			for (mreg r : regs_in(b.df_out_live))
				printf(" %s", r.to_string().c_str());
			printf("\n");
			printf("Def = ");
			for (mreg r : regs_in(b.df_def))
				printf(" %s", r.to_string().c_str());
			printf("\n");
			printf("Ref = ");
			for (mreg r : regs_in(b.df_ref))
				printf(" %s", r.to_string().c_str());
			printf("\n");

			for (auto& i : b.instructions) {
				printf("\t%s ", i.to_string().c_str());

				i.for_each_reg([&](const mreg& m, bool) {
					if (gr.size() > m.uid()) {
						printf("|I[%s]:", m.to_string().c_str());
						for (mreg r : regs_in(gr[m.uid()].vtx))
							if (r != m)
								printf(" %s", r.to_string().c_str());
					}
				});
				printf("\n");
			}
		}
	}

	// Tries coloring the graph with K+M colors respectively for GP and FP.
	//
	static std::pair<size_t, size_t> try_color(std::span<graph_node> gr, size_t K, size_t M) {
		// Pick a node we can simplify with < K/M nodes.
		//
		graph_node* overlimit_it = nullptr;
		graph_node* it           = nullptr;
		for (auto& n : gr) {
			// Skip disconnected and pre-colored nodes.
			//
			if (n.color)
				continue;
			size_t deg = n.vtx.popcount();
			if (deg == 0)
				continue;
			--deg;

			// If over the limit, set as spill iterator using the weight.
			//
			if (deg > (n.is_fp ? M : K)) {
				if (!overlimit_it || overlimit_it->priority > n.priority) {
					overlimit_it = &n;
				}
			}
			// Otherwise, set to color.
			//
			else {
				it = &n;
				break;
			}
		}

		// If we did not find any:
		//
		if (!it) {
			// If none left, success.
			//
			if (!overlimit_it)
				return {0, 0};

			// Otherwise continue as spilled.
			//
			it = overlimit_it;
		}

		// Remove from the graph.
		//
		util::bitset tmp{gr.size()};
		tmp.swap(it->vtx);
		for (size_t i = 0; i != gr.size(); i++) {
			if (tmp[i])
				gr[i].vtx.reset(it - gr.data());
		}

		// Recursively color.
		//
		auto [rec_spill_gp, rec_spill_fp] = try_color(gr, K, M);

		// Add the node back.
		//
		size_t color_mask = ~0ull;
		for (size_t i = 0; i != gr.size(); i++) {
			if (tmp[i]) {
				gr[i].vtx.set(it - gr.data());
				if (gr[i].color != 0 && i != (it - gr.data())) {
					color_mask &= ~(1ull << (gr[i].color - 1));
				}
			}
		}
		tmp.swap(it->vtx);

		// Try using any coalescing hints.
		//
		for (auto hint_off : it->coalescing_hints) {
			if (!hint_off)
				continue;
			auto hint = it + hint_off;
			if (hint->color) {
				if (color_mask & (1ull << (hint->color - 1))) {
					it->color = hint->color;
					return {rec_spill_gp, rec_spill_fp};
				}
			}
		}

		// Pick a color.
		//
		size_t n = std::countr_zero(color_mask);
		if (n > (it->is_fp?M:K)) {
			// Increment spill counter.
			//
			if (it->is_fp)
				rec_spill_fp++;
			else
				rec_spill_gp++;

			// Find spill slot.
			//
			it->color = 0;
			it->spill_slot = 1;
			bool changed   = false;
			do {
				changed = false;
				for (size_t i = 0; i != gr.size(); i++) {
					if (gr[i].spill_slot == it->spill_slot && it != &gr[i] && gr[i].vtx[it - gr.data()]) {
						it->spill_slot++;
						changed = true;
						break;
					}
				}
			} while (changed);
		} else {
			it->color = n + 1;
		}
		return {rec_spill_gp, rec_spill_fp};
	}

	// Spills all arguments into virtual registers.
	//
	static void spill_args(mprocedure* proc) {
		// Before anything else, spill all arguments into virtual registers.
		//
		mreg regs[3] = {};
		for (auto& bb : proc->basic_blocks) {
			for (auto& i : bb.instructions) {
				i.for_each_reg([&](const mreg& r, bool is_read) {
					mreg* replace_with = nullptr;
					if (r == vreg_vm)
						replace_with = &regs[0];
					else if (r == vreg_tos)
						replace_with = &regs[1];
					else if (r == vreg_nargs)
						replace_with = &regs[2];
					if (replace_with) {
						if (replace_with->is_null()) {
							*replace_with = proc->next_gp();
						}
						const_cast<mreg&>(r) = *replace_with;
					}
				});
			}
		}
		for (size_t i = 0; i != std::size(regs); i++) {
			if (regs[i]) {
				proc->basic_blocks.front().instructions.insert(proc->basic_blocks.front().instructions.begin(), minsn{vop::movi, regs[i], mreg(arch::map_argument(i, 0, false))});
			}
		}
	}

	// Does lifetime analysis and builds the interference graph.
	//
	static std::vector<graph_node> build_graph(mprocedure* proc, std::vector<graph_node>* tmp = nullptr) {
		// Get maximum register id and calculate use counts.
		//
		std::vector<size_t> reg_use_counter = {};
		uint32_t            max_reg_id      = 0;
		for (auto& bb : proc->basic_blocks) {
			for (auto& i : bb.instructions) {


				i.for_each_reg([&](mreg r, bool is_read) {
					if (max_reg_id < r.uid()) {
						max_reg_id = r.uid();
						reg_use_counter.resize(max_reg_id + 1);
					}
					if (is_read) {
						reg_use_counter[r.uid()]++;
					}

					if (i.is(vop::loadi64) || i.is(vop::storei64) || i.is(vop::loadf64) || i.is(vop::storef64))
						reg_use_counter[r.uid()] += 100;
				});
			}
		}
		++max_reg_id;

		// First calculate ref(n) and def(n) for each basic block.
		//
		for (auto& bb : proc->basic_blocks) {
			bb.df_def.clear();
			bb.df_ref.clear();
			bb.df_in_live.clear();
			bb.df_out_live.clear();
			bb.df_def.resize(max_reg_id);
			bb.df_ref.resize(max_reg_id);
			bb.df_in_live.resize(max_reg_id);
			bb.df_out_live.resize(max_reg_id);

			for (auto& i : bb.instructions) {
				i.for_each_reg([&](mreg r, bool is_read) {
					if (is_pseudo(r))
						return;

					if (is_read) {
						if (!bb.df_def[r.uid()]) {
							bb.df_ref.set(r.uid());
						}
					} else {
						bb.df_def.set(r.uid());
					}
				});
			}
		}

		// Calculate in-live ranges:
		// - in-live(n) = (out-live(n)\def(n)) U ref(n)
		// - out-live(n) = for each succ, (... U in-live(s))
		//
		bool changed;
		do {
			changed = false;

			// For each block:
			//
			for (auto& bb : proc->basic_blocks) {
				util::bitset new_live{max_reg_id};
				for (auto& s : bb.successors) {
					new_live.set_union(s->df_in_live);
				}
				new_live.set_difference(bb.df_def);
				new_live.set_union(bb.df_ref);
				if (new_live != bb.df_in_live) {
					changed = true;
					new_live.swap(bb.df_in_live);
				}
			}
		} while (changed);

		// Convert to out-live.
		//
		for (auto& b : proc->basic_blocks) {
			for (auto& suc : b.successors)
				b.df_out_live.set_union(suc->df_in_live);
		}

		// Allocate the interference graph and set the initial state.
		//
		std::vector<graph_node> interference_graph;
		if (tmp) {
			interference_graph = std::move(*tmp);
			interference_graph.clear();
		}
		interference_graph.resize(max_reg_id);
		for (size_t i = 0; i != max_reg_id; i++) {
			auto& node = interference_graph[i];
			auto  mr   = mreg::from_uid(i);
			node.vtx.resize(max_reg_id);
			node.vtx.set(i);
			node.priority = (reg_use_counter[i] + 1) * RA_PRIO_HOT_BIAS;
			node.is_fp    = mr.is_fp();
			if (mr.is_phys()) {
				node.color = std::abs(mr.phys());
			}
		}

		// Build the interference graph.
		//
		auto add_vertex = [&](mreg a, mreg b) {
			if (!interferes_with(a, b))
				return true;
			auto au   = a.uid();
			auto bu   = b.uid();
			bool prev = interference_graph[au].vtx.set(bu);
			interference_graph[bu].vtx.set(au);
			return prev;
		};
		auto add_set = [&](const util::bitset& b, mreg def) {
			for (size_t i = 0; i != max_reg_id; i++) {
				if (b[i]) {
					add_vertex(def, mreg::from_uid(i));
				}
			}
		};
		for (auto& b : proc->basic_blocks) {
			auto live = b.df_out_live;
			for (auto& i : view::reverse(b.instructions)) {
				if (i.is(vop::movi) || i.is(vop::movf)) {
					if (i.arg[0].is_reg()) {
						auto& a = interference_graph[i.arg[0].reg.uid()];
						auto& b = interference_graph[i.out.uid()];
						a.add_hint(&b);
						b.add_hint(&a);
					}
				}

				if (i.out) {
					live.reset(i.out.uid());
					add_set(live, i.out);
				}

				i.for_each_reg([&](mreg r, bool is_read) {
					if (is_read) {
						live.set(r.uid());
					}
				});
				i.for_each_reg([&](mreg r, bool is_read) {
					if (is_read) {
						add_set(live, r);
					}
				});
			}
		}
		return interference_graph;
	}

	// Allocates registers for each virtual register and generates the spill instructions.
	//
	void allocate_registers(mprocedure* proc) {
		// Spill arguments.
		//
		spill_args(proc);

		// Build the interference graph.
		//
		std::vector<graph_node> interference_graph = build_graph(proc);
		print_graph(interference_graph);
		print_lifetime(proc);

		// Enter the register allocation loop.
		//
		static constexpr size_t MAX_K =  arch::num_gp_reg;
		static constexpr size_t MAX_M = arch::num_fp_reg;
		size_t                  K     = std::min(MAX_K, std::max<size_t>(std::size(arch::gp_volatile), 2));
		size_t                  M     = std::min(MAX_M, std::max<size_t>(std::size(arch::fp_volatile), 2));
		std::vector<graph_node> interference_graph_copy(interference_graph);

		int32_t num_spill_slots = 0;
		for(size_t step = 0;; step++) {
			LI_ASSERT(step < 32);

			// Try coloring the graph.
			//
			auto [spill_gp, spill_fp] = try_color(interference_graph, K, M);
			printf("Try_color (K=%llu, M=%llu) spills (%llu, %llu) registers\n", K, M, spill_gp, spill_fp);
			//print_lifetime(proc);

			// If we don't need to spill, break out.
			//
			if (!spill_gp && !spill_fp) {
				break;
			}

			// If we have more registers to allocate, restore old graph and try again.
			//
			bool increase_k = spill_gp && K != MAX_K;
			bool increase_m = spill_fp && M != MAX_M;
			K += increase_k ? 1 : 0;
			M += increase_m ? 1 : 0;
			if (increase_k || increase_m) {
				interference_graph = interference_graph_copy;
				continue;
			}

			// Add spilling code.
			//
			struct spill_entry {
				mreg   src  = {};
				mreg   dst  = {};
				int32_t slot = 0;
			};
			int32_t slot_offset = num_spill_slots;
			for (auto& bb : proc->basic_blocks) {
				for (auto it = bb.instructions.begin(); it != bb.instructions.end();) {
					spill_entry reload_list[4] = {};
					spill_entry spill_list[1]  = {};
					bool        need_cg        = false;

					auto spill_and_swap = [&](mreg& m, std::span<spill_entry> list, size_t slot) {
						need_cg = true;
						for (auto& entry : list) {
							if (entry.src.is_null()) {
								entry.src  = m;
								entry.dst  = m.is_fp() ? proc->next_fp() : proc->next_gp();
								entry.slot = slot + slot_offset - 1;
								m          = entry.dst;
								num_spill_slots = std::max(num_spill_slots, entry.slot + 1);
								break;
							} else if (entry.src == m) {
								m = entry.dst;
								break;
							}
						}
						assume_unreachable();
					};
					it->for_each_reg([&](const mreg& r, bool is_read) {
						if (is_pseudo(r) || !r.is_virt())
							return false;
						if (interference_graph.size() <= r.uid())
							return false;

						// Skip if not spilled.
						//
						auto& info = interference_graph[r.uid()];
						if (!info.spill_slot) {
							return false;
						}
						if (is_read) {
							spill_and_swap(const_cast<mreg&>(r), reload_list, info.spill_slot);
						} else {
							spill_and_swap(const_cast<mreg&>(r), spill_list, info.spill_slot);
						}
						return false;
					});

					// If we don't need to change anything, continue.
					//
					if (!need_cg) {
						++it;
						continue;
					}

					// Reload and spill as requested.
					//
					for (auto& entry : reload_list) {
						if (entry.src.is_null())
							break;
						auto op = entry.src.is_fp() ? vop::loadf64 : vop::loadi64;
						mmem mem{.base = arch::from_native(arch::sp), .disp = entry.slot * 8};
						it = bb.instructions.insert(it, minsn{op, entry.dst, mem}) + 1;
					}
					for (auto& entry : spill_list) {
						if (entry.src.is_null())
							break;
						auto op = entry.src.is_fp() ? vop::storef64 : vop::storei64;
						mmem mem{.base = arch::from_native(arch::sp), .disp = entry.slot * 8};
						it = bb.instructions.insert(it + 1, minsn{op, {}, mem, entry.dst}) + 1;
					}
				}
			}

			// Rebuild the interference graph.
			//
			interference_graph = build_graph(proc, &interference_graph);
			interference_graph_copy = interference_graph;
		}
		proc->used_stack_length = ((num_spill_slots + 1) & ~1) * 8;

		// Swap the registers in the IR.
		//
		for (auto& bb : proc->basic_blocks) {
			for (auto& i : bb.instructions) {
				i.for_each_reg([&](const mreg& r, bool is_read) {
					if (!is_pseudo(r) && r.is_virt()) {
						int x = int(interference_graph[r.uid()].color);
						LI_ASSERT(x != 0);

						if (r.is_fp()) {
							proc->used_fp_mask |= 1ull << (x - 1);
							x = -x;
						} else {
							proc->used_gp_mask |= 1ull << (x - 1);
						}
						const_cast<mreg&>(r) = arch::reg(x);
					}
					return false;
				});
			}
		}
		proc->print();

		// Remove eliminated moves.
		//
		for (auto& bb : proc->basic_blocks) {
			std::erase_if(bb.instructions, [](minsn& i) {
				if (i.is(vop::movf) || i.is(vop::movi)) {
					if (i.arg[0].is_reg())
						return i.out == i.arg[0].reg;
				}
				return false;
			});
		}
	}
};