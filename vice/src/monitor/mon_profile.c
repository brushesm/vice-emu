/*
 * mon_profiler.c -- Monitor Interface for CPU profiler
 *
 * Written by
 *  Oskar Linde <oskar.linde@gmail.com>
 *
 * This file is part of VICE, the Versatile Commodore Emulator.
 * See README for copyright notice.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 *  02111-1307  USA.
 *
 */

#include "vice.h"

#include <stdio.h>
#include <string.h>

#include "archdep.h"
#include "lib.h"
#include "machine.h"
#include "maincpu.h"
#include "mon_disassemble.h"
#include "mon_profile.h"
#include "profiler.h"
#include "profiler_data.h"

const int min_label_width = 15;
static void print_disass_context(profiling_context_t *context, bool print_subcontexts);
static void print_function_line(profiling_context_t *context, int indent, profiling_counter_t total_cycles);

enum ContextType {
    CONTEXT_ALIASED = 1,
    CONTEXT_MERGED  = 2,
};

static bool is_aliased(profiling_context_t *context) {
    return context->pc_src == 0 /* aggregate */ && context->id & CONTEXT_ALIASED;
}

static bool is_merged(profiling_context_t *context) {
    return context->pc_src == 0 /* aggregate */ && context->id & CONTEXT_MERGED;
}


void mon_profile(void)
{
    if (maincpu_profiling) {
        mon_out("Profiling running.\n");
    } else if (!root_context) {
        mon_out("Profiling not started.\n");
    } else {
        mon_out("Profiling data available.\n");
    }
    mon_out("Use \"help prof\" for more information.\n");
}

void mon_profile_action(ACTION action)
{
    switch(action) {
    case e_OFF: {
        if (maincpu_profiling) {
            profile_stop();
            mon_out("Profiling stopped.\n");
        } else {
            mon_out("Profiling not started.\n");
        }
        return;
    }
    case e_ON: {
        profile_start();
        if (maincpu_profiling) {
            mon_out("Profiling restarted.\n");
        } else {
            mon_out("Profiling started.\n");
        }
        return;
    }
    case e_TOGGLE: {
        if (maincpu_profiling) {
            mon_profile_action(e_OFF);
        } else {
            mon_profile_action(e_ON);
        }
        return;
    }
    }
}

static bool init_profiling_data(void) {
    if (!root_context) {
        mon_out("No profiling data available. Start profiling with \"prof on\".\n");
        return false;
    }

    compute_aggregate_stats(root_context);

    return true;
}

static void merge_aggregate_contexts(profiling_context_t *output,
                                     profiling_context_t *source) {
    int i,j;

    if (output->memory_bank_config != source->memory_bank_config) {
        /* mark that the context is a merge of multiple memory configs */
        output->id |= CONTEXT_MERGED;
    }

    output->num_enters               += source->num_enters;
    output->num_exits                += source->num_exits;
    output->total_cycles             += source->total_cycles;
    output->total_cycles_self        += source->total_cycles_self;
    output->total_stolen_cycles      += source->total_stolen_cycles;
    output->total_stolen_cycles_self += source->total_stolen_cycles_self;

    while (source) {
        profiling_context_t *output_context = get_mem_config_context(output, source->memory_bank_config);
        for (i = 0; i < 256; i++) {
            if (source->page[i]) {
                profiling_page_t *output_page = profiling_get_page(output_context, i);
                profiling_page_t *source_page = source->page[i];
                for (j = 0; j < 256; j++) {
                    output_page->data[j].num_cycles  += source_page->data[j].num_cycles;
                    output_page->data[j].num_samples += source_page->data[j].num_samples;
                    output_page->data[j].touched     |= source_page->data[j].touched;
                }
            }
        }

        source = source->next_mem_config;
    }
}

/* check to see if two aggregates seem to be collapsible
 * i.e. even if they have different memory banking configurations
 * memory areas referenced by the pc are identical */
static bool are_aggregates_compatible(profiling_context_t *a,
                                      profiling_context_t *b) {
    if (a->memory_bank_config == b->memory_bank_config) {
        return true;
    }

    int i;
    MEMSPACE mem = e_comp_space;
    uint16_t loc;

    /* loop over all memory configs within each context */
    profiling_context_t *ac = a;
    while (ac) {
        profiling_context_t *bc = b;
        while (bc) {

            for (i = 0; i < 256; i++) {
                profiling_page_t *page_a = ac->page[i];
                profiling_page_t *page_b = bc->page[i];

                if (page_a && page_b) {
                    int j;

                    for (j = 0; j < 256; j++) {
                        if (page_a->data[j].num_samples > 0 &&
                            page_b->data[j].num_samples > 0) {
                            int k;

                            loc = (i << 8) | j;

                            /* check the pc addr and the next 2
                             * memory locations for opcodes up to length 3 */
                            for (k = 0; k < 3; k++) {
                                if (mon_get_mem_val_nosfx(mem, ac->memory_bank_config, loc + k) !=
                                    mon_get_mem_val_nosfx(mem, bc->memory_bank_config, loc + k)) {
                                    /* incompatible memory bank configs */
                                    return false;
                                }
                            }
                        }
                    }
                }
            }
            bc = bc->next_mem_config;
        }
        ac = ac->next_mem_config;
    }

    return true;
}

typedef struct context_array_s {
    profiling_context_t **data;
    int capacity;
    int size;
} context_array_t;

typedef struct aggregate_stats_s {
    profiling_context_t *aggregate;
    context_array_t callers;
    context_array_t callees;
    struct aggregate_stats_s *next;
} aggregate_stats_t;

static uint16_t parent_function(profiling_context_t *context) {
    if (context->parent) {
        return context->parent->pc_dst;
    } else {
        return 0x0000;
    }
}

/* acending based on pc_dst */
static int pc_dst_compare(void const* a, void const* b) {
    return (int)(*((profiling_context_t**)a))->pc_dst - (int)(*((profiling_context_t**)b))->pc_dst;
}

/* acending based on pc_dst */
static int sort_by_parent_function(void const* a, void const* b) {
    return (int)parent_function(*((profiling_context_t**)a)) - (int)parent_function(*((profiling_context_t**)b));
}

/* descending based on total_cycles_self */
static int self_time(void const* a, void const* b) {
    return (int)(*((profiling_context_t**)b))->total_cycles_self - (int)(*((profiling_context_t**)a))->total_cycles_self;
}

/* descending based on total_cycles */
static int total_time(void const* a, void const* b) {
    return (int)(*((profiling_context_t**)b))->total_cycles - (int)(*((profiling_context_t**)a))->total_cycles;
}


static void array_sort(context_array_t *arr, int (*compar)(const void *, const void *))
{
    qsort(arr->data, arr->size, sizeof(*arr->data), compar);
}

static int binary_search(context_array_t *arr, profiling_context_t *ref, int (*compar)(const void *, const void *))
{
    int lo = 0;
    int hi = arr->size;
    while (hi > lo) {
        int mid = (lo+hi)/2;
        int res = compar(&arr->data[mid], &ref);
        if (res < 0) {
            lo = mid+1;
        } else if (res > 0) {
            hi = mid;
        } else {
            /* find smallest matching index in case we have duplicates */
            while (mid > lo) {
                /* check previous element */
                if (compar(&arr->data[mid-1], &ref) < 0) {
                    /* found smallest index */
                    return mid;
                }
                mid--;
            }
            return mid;
        }
    }

    return arr->size;
}

static void array_append(context_array_t *arr, profiling_context_t *context) {
    arr->size++;

    if (arr->size > arr->capacity) {
        arr->capacity *= 2;
        if (arr->capacity < 10) {
            arr->capacity = 10;
        }
        arr->data = lib_realloc(arr->data, arr->capacity * sizeof(*arr->data));
    }

    arr->data[arr->size-1] = context;
}

static void array_free(context_array_t *arr) {
    lib_free(arr->data);
    arr->data = NULL;
    arr->size = 0;
    arr->capacity = 0;
}

static aggregate_stats_t *alloc_aggregate_stats(void) {
    /* zeroes out all fields */
    return lib_calloc(1, sizeof(aggregate_stats_t));
}

static void free_aggregate_stats(aggregate_stats_t *c) {
    if (!c) return;

    if (c->next) {
        free_aggregate_stats(c->next);
    }
    free_profiling_context(c->aggregate);
    array_free(&c->callers);
    array_free(&c->callees);
    lib_free(c);
}

static void mark_aliases(context_array_t *contexts) {
    for (int i = 1; i < contexts->size; i++) {
        if (contexts->data[i  ]->pc_dst ==
            contexts->data[i-1]->pc_dst) {
            /* mark that the contexts are aliased */
            contexts->data[i  ]->id |= CONTEXT_ALIASED;
            contexts->data[i-1]->id |= CONTEXT_ALIASED;
        }
    }
}

/* Aggregate stats for all context that match call address 'addr'
 * If multiple incompatible contexts exist, they get separated out into different
 * aggregates.
 * Returning data by populating a linked list of incompatible aggregates with
 * their respective caller/callee contexts.
 */
static void recursively_aggregate_matching_functions(
    profiling_context_t *query_context,
    uint16_t             addr,
    aggregate_stats_t  **stat_list)
{
    aggregate_stats_t *matched_stats = NULL;

    if (query_context->pc_dst == addr) {
        /* find matching call_list context */
        matched_stats = *stat_list;
        while(matched_stats) {
            if (are_aggregates_compatible(query_context, matched_stats->aggregate)) {
                break; /* match found */
            }
            matched_stats = matched_stats->next;
        }
        if (!matched_stats) {
            /* initialize new stats aggregate */
            aggregate_stats_t *new_stats = alloc_aggregate_stats();
            new_stats->aggregate = alloc_profiling_context();
            new_stats->aggregate->pc_dst = addr;
            new_stats->aggregate->memory_bank_config = query_context->memory_bank_config;

            new_stats->next = *stat_list;
            *stat_list = new_stats;
            matched_stats = *stat_list;
        }
    }

    if (matched_stats) {
        merge_aggregate_contexts(matched_stats->aggregate, query_context);
        array_append(&matched_stats->callers, query_context);
    }

    if (query_context->child) {
        profiling_context_t *c = query_context->child;
        do {
            if (matched_stats) {
                array_append(&matched_stats->callees, c);
            }
            recursively_aggregate_matching_functions(c, addr, stat_list);
            c = c->next;
        } while (c != query_context->child);
    }
}

static void recursively_aggregate_all_functions(profiling_context_t *query_context,
                                                context_array_t *all_functions) {
    /* binary search for matching context */
    int element = binary_search(all_functions, query_context, pc_dst_compare);
    bool match_found = false;

    /* check all matches to see if any is compatible */
    while (element < all_functions->size) {
        if (query_context->pc_dst != all_functions->data[element]->pc_dst) {
            break;
        }
        if (are_aggregates_compatible(query_context, all_functions->data[element])) {
            match_found = true;
            merge_aggregate_contexts(all_functions->data[element], query_context);
            break;
        }

        element++;
    }

    if (!match_found) {
        /* no match, insert and sort */
        profiling_context_t *new_context = alloc_profiling_context();

        new_context->pc_dst = query_context->pc_dst;
        new_context->memory_bank_config = query_context->memory_bank_config;
        merge_aggregate_contexts(new_context, query_context);
        array_append(all_functions, new_context);
        array_sort(all_functions, pc_dst_compare);
    }

    /* recursively add children */
    if (query_context->child) {
        profiling_context_t *c = query_context->child;
        do {
            recursively_aggregate_all_functions(c, all_functions);
            c = c->next;
        } while (c != query_context->child);
    }
}

/* assuming sorted source array */
static void array_compact(context_array_t * output, context_array_t const* source, int (*compar)(const void *, const void *)) {
    int i;

    for (i = 0; i < source->size; i++) {
        int j;
        profiling_context_t *aggregate = alloc_profiling_context();

        aggregate->pc_dst = source->data[i]->pc_dst;
        aggregate->memory_bank_config = source->data[i]->memory_bank_config;
        /* HACK, store parent->dst as source pointer */
        aggregate->pc_src = parent_function(source->data[i]);
        merge_aggregate_contexts(aggregate, source->data[i]);

        /* merge all compatible contexts */
        for (j = i+1; j < source->size; j++) {
            /* i points to the last compatible context */
            if (compar(&source->data[i], &source->data[j]) != 0) {
                break;
            }
            if (are_aggregates_compatible(aggregate, source->data[j])) {
                merge_aggregate_contexts(aggregate, source->data[j]);
                /* swap places with data[i] to remove any gaps */
                i++; /* guaranteed to still be < size */
                profiling_context_t *tmp = source->data[i];
                source->data[i] = source->data[j];
                source->data[j] = tmp;
            }
        }

        array_append(output, aggregate);
    }
}

void mon_profile_flat(int num)
{
    int i;

    if (!init_profiling_data()) return;

    if (num <= 0) num = 20;

    context_array_t all_functions;

    all_functions.size = 0;
    all_functions.capacity = 0;
    all_functions.data = NULL;

    recursively_aggregate_all_functions(root_context, &all_functions);
    mark_aliases(&all_functions);

    /* sort based on self time */
    array_sort(&all_functions, self_time);

    mon_out("        Total      %%          Self      %%\n");
    mon_out("------------- ------ ------------- ------\n");

    if (num > all_functions.size) num = all_functions.size;
    for (i = 0; i < num; i++) {
        print_function_line(all_functions.data[i], 0 /* indent */, root_context->total_cycles);
        free_profiling_context(all_functions.data[i]);
    }
    for (i = num; i < all_functions.size; i++) {
        free_profiling_context(all_functions.data[i]);
    }

    array_free(&all_functions);
}


static void print_context_graph(profiling_context_t *context, int depth, int max_depth, profiling_counter_t total_cycles);


void mon_profile_graph(int context_id, int depth)
{
    profiling_context_t * context;

    if (!init_profiling_data()) return;

    context = profile_context_by_id(context_id);
    if (!context) {
        context = root_context;
    }

    if (depth <= 0) depth = 4;

    mon_out("%*s        Total      %%          Self      %%\n", min_label_width + 15 + depth*2+1, "");
    mon_out("%*s------------- ------ ------------- ------\n", min_label_width + 15 + depth*2+1, "");

    print_context_graph(context, 0, depth, context->total_cycles);
}


void mon_profile_func(MON_ADDR function)
{
    aggregate_stats_t *func_stats = NULL;
    aggregate_stats_t *c = NULL;

    uint16_t addr;
    int i;

    if (!init_profiling_data()) return;

    if (addr_memspace(function) == e_default_space) {
        addr = addr_location(function);
    } else {
        mon_out("Invalid address space\n");
        return;
    }

    recursively_aggregate_matching_functions(root_context, addr, &func_stats);
    c = func_stats;
    while(c) {
        context_array_t callers_merged = {NULL, 0, 0};
        context_array_t callees_merged = {NULL, 0, 0};

        /* merge all callers that have the same parent function */
        array_sort(&c->callers, sort_by_parent_function);
        array_compact(&callers_merged, &c->callers, sort_by_parent_function);
        array_sort(&callers_merged, self_time);

        /* merge all calleess that have the same target function */
        array_sort(&c->callees, pc_dst_compare);
        array_compact(&callees_merged, &c->callees, pc_dst_compare);
        array_sort(&callees_merged, total_time);
        mon_out("                                          Callers\n");
        mon_out("        Total      %%          Self      %%         Callees\n");
        mon_out("------------- ------ ------------- ------ |---|---|------\n");

        for (i = 0; i < callers_merged.size; i++) {
            /* HACK: the function name is stored in pc_src */
            callers_merged.data[i]->pc_dst = callers_merged.data[i]->pc_src;
            callers_merged.data[i]->pc_src = 0;
            print_function_line(callers_merged.data[i], 0 /* indent */, c->aggregate->total_cycles);
            free_profiling_context(callers_merged.data[i]);
        }

        if (func_stats->next) {
            /* more than one context */
            c->aggregate->id |= CONTEXT_ALIASED;
        }
        print_function_line(c->aggregate, 4 /* indent */, c->aggregate->total_cycles);

        for (i = 0; i < callees_merged.size; i++) {
            print_function_line(callees_merged.data[i], 8 /* indent */, c->aggregate->total_cycles);
            free_profiling_context(callees_merged.data[i]);
        }

        array_free(&callers_merged);
        array_free(&callees_merged);

        c = c->next;
    }

    free_aggregate_stats(func_stats);
}

static bool is_interrupt(uint16_t src) {
    return src >= 0xfffa && !(src & 1);
}

static void print_src(uint16_t src) {
    switch(src) {
    case 0x0000: mon_out("BOOT"); return;
    case 0xfffa: mon_out("NMI "); return;
    case 0xfffc: mon_out("RST "); return;
    case 0xfffe: mon_out("IRQ "); return;
    /* subtract 2 to show the start of the JSR instruction */
    default: mon_out("%04x", (unsigned)(src-2)); return;
    }
}

/* memory_config -2 will print "{*}" */
static void print_dst(uint16_t dst, int max_width, int memory_config) {
    char *name = mon_symbol_table_lookup_name(default_memspace, dst);
    char buf[32];
    char *full_name = NULL;
    size_t l;

    if (!name) {
        if (dst == 0x0000) {
            snprintf(buf, 32, "ROOT");
        } else {
            snprintf(buf, 32, "%04x", dst);
        }
        name = buf;
    }

    l = strlen(name);

    if (memory_config != -1) {
        char suffix[32];
        if (memory_config >= 0) {
            snprintf(suffix, 32, " {%d}", memory_config);
        } else {
            snprintf(suffix, 32, " {*}");
        }
        full_name = lib_calloc(l + strlen(suffix) + 1, 1);
        /* strncpy(full_name, name, l); */
        strcat(full_name, name); /* since name was not changed, this will always copy max l bytes */
        strcat(full_name, suffix);
        name = full_name;
        l += 1 + strlen(suffix);
    }

    if (l > max_width) {
        mon_out("%*.*s...", -(max_width-3), (max_width-3), name);
    } else {
        mon_out("%*s", -max_width, name);
    }

    lib_free(full_name);
}

static int context_memory_config(profiling_context_t *context) {
    return is_aliased(context)
               ? is_merged(context) /* multiple configs merged into one */
                     ? -2
                     : context->memory_bank_config
               : -1 /* print no suffix */;
}

static void print_context_id(profiling_context_t *context, int max_width) {
    char idstr[16];
    snprintf(idstr, 16, "[%d]", get_context_id(context));
    mon_out("%*s", max_width, idstr);
}

static void print_context_name(profiling_context_t *context, int indent, int max_indent) {
    print_context_id(context, indent + 6);
    mon_out(" ");
    if (context->pc_dst == 0) {
        mon_out("START   ");
        mon_out("%*s", min_label_width + max_indent - indent, "");
    } else {
        print_src(context->pc_src);
        mon_out(" -> ");
        print_dst(context->pc_dst, min_label_width + max_indent - indent,
                  context_memory_config(context));
    }
}

static void print_function_line(profiling_context_t *context, int indent, profiling_counter_t total_cycles) {
    mon_out("%'13u %5.1f%% ", context->total_cycles,      100.0 * context->total_cycles / total_cycles);
    mon_out("%'13u %5.1f%% ", context->total_cycles_self, 100.0 * context->total_cycles_self / total_cycles);
    mon_out("%*s", indent, "");
    print_dst(context->pc_dst, 40, context_memory_config(context));

    mon_out("\n");
}

static void print_context_line(profiling_context_t *context, int indent, int max_indent, profiling_counter_t total_cycles) {
    print_context_name(context, indent, max_indent);

    mon_out("%'13u %5.1f%% ",  context->total_cycles,      100.0 * context->total_cycles / total_cycles);
    mon_out("%'13u %5.1f%%\n", context->total_cycles_self, 100.0 * context->total_cycles_self / total_cycles);
}

static void print_context_graph(profiling_context_t *context, int depth, int max_depth, profiling_counter_t total_cycles)
{
    if (context == current_context) {
        mon_out(">");
    } else {
        mon_out(" ");
    }

    print_context_line(context, depth * 2, max_depth * 2, total_cycles);

    if (context->child) {
        if (depth < max_depth)  {
            profiling_context_t *c = context->child;
            do {
                print_context_graph(c, depth + 1, max_depth, total_cycles);
                c = c->next;
            } while (c != context->child);
        } else {
            /* check if we are a child to the current_context */
            profiling_context_t *c = current_context;
            do {
                c = c->parent;
                if (c == context) {
                    /* if so, print cursor */
                    mon_out(">");
                    break;
                }
            } while(c);

            if (!c) {
                mon_out(" ");
            }

            mon_out("%*s...\n", (depth+1)*2 + 8, "");
        }
    }
}

static void print_all_contexts(context_array_t *context_list)
{
    for (int i = 0; i < context_list->size; i++) {
        mon_out("[%d]", get_context_id(context_list->data[i]));
    }
}

void mon_profile_disass(MON_ADDR function)
{
    aggregate_stats_t *func_stats = NULL;
    aggregate_stats_t *c = NULL;

    uint16_t addr;

    if (!init_profiling_data()) return;

    if (addr_memspace(function) == e_default_space) {
        addr = addr_location(function);
    } else {
        mon_out("Invalid address space\n");
        return;
    }

    recursively_aggregate_matching_functions(root_context, addr, &func_stats);

    c = func_stats;
    while(c) {
        mon_out("\n");

        mon_out("Function ");
        print_dst(addr, 70, func_stats->next /* more than one? */
                                ? is_merged(c->aggregate)
                                      ? -2 /* magic for print '*' */
                                      : c->aggregate->memory_bank_config
                                : -1 /* magic for print nothing */
                  );
        mon_out("\n");

        mon_out("\n");

        mon_out("   Contexts ");
        print_all_contexts(&c->callers);
        mon_out("\n");
        mon_out("   Subcontexts ");
        print_all_contexts(&c->callees);
        mon_out("\n");

        mon_out("\n");

        print_disass_context(c->aggregate, false /* print_subcontexts */);

        c = c->next;
    }

    free_aggregate_stats(func_stats);
}

static void print_cycle_time(double cycles, int align_column) {
    double time = cycles / machine_get_cycles_per_second();
    char  *unit;
    char  *format = "%*.*f %s";
    int    precision;

    /* try to maintain 3 significant figures */

    if (time >= 1.0) {
        unit = "s";
    } else if (time >= 1e-3) {
        unit = "ms";
        time *= 1e3;
    } else {
        unit = "us";
        time *= 1e6;
    }

    if      (time >= 100)  precision = 0;
    else if (time >= 10)   precision = 1;
    else                   precision = 2;

    mon_out(format, align_column, precision, time, unit);
}

static void merge_all_pages(profiling_context_t *target, profiling_context_t *source) {
    int i, j;

    for (i = 0; i < 256; i++) {
        profiling_page_t *source_page = source->page[i];
        if (source_page) {
            /* initialize target page if it doesn't exist */
            profiling_page_t *target_page = profiling_get_page(target, i);

            for (j = 0; j < 256; j++) {
                target_page->data[j].num_cycles  += source_page->data[j].num_cycles;
                target_page->data[j].num_samples += source_page->data[j].num_samples;
                target_page->data[j].touched     |= source_page->data[j].touched;
            }
        }
    }
}

static void merge_all_memory_configs(profiling_context_t *context) {
    profiling_context_t *c = context->next_mem_config;
    while (c) {
        profiling_context_t *c_next;
        merge_all_pages(context, c);

        /* free the memory config */
        c_next = c->next_mem_config;
        c->next_mem_config = NULL;
        free_profiling_context(c);
        c = c_next;
    }

    context->next_mem_config = NULL;
}


static bool is_branch_instruction(uint16_t mem_config, uint16_t addr, unsigned *opc_size, uint16_t *dest_addr) {
    /* on 6502, an instruction is a conditional branch if the opcode is
     * $x0, where x is odd */
    uint8_t opc  = mon_get_mem_val_nosfx(e_comp_space, mem_config, addr);
    int8_t  offset;

    if ((opc & 0x1f) != 0x10) {
        return false;
    }

    *opc_size = 2;

    offset = mon_get_mem_val_nosfx(e_comp_space, mem_config, addr+1);

    *dest_addr = (uint16_t)(addr + *opc_size + offset);
    return true;
}


static void print_disass_context(profiling_context_t *context, bool print_contexts) {
    profiling_context_t *c;
    int num_memory_map_configs = 0;
    int i, j;
    uint16_t next_addr = 0;
    double average_times;
    int context_column = 0;
    int addr_column = 0;

    average_times = 0.5 * (context->num_enters + context->num_exits);

    mon_out("   Entered        %'10u time%s\n", context->num_enters, context->num_enters != 1 ? "s":"");
    mon_out("   Exited         %'10u time%s\n", context->num_exits,  context->num_exits  != 1 ? "s":"");

    mon_out("   Total          %'10u cycles ", context->total_cycles);
    print_cycle_time(context->total_cycles, 10);
    mon_out("\n");

    mon_out("   Self           %'10u cycles ", context->total_cycles_self);
    print_cycle_time(context->total_cycles_self, 10);
    mon_out("\n");

    mon_out("   Average        %'10.0f cycles ", context->total_cycles/average_times);
    print_cycle_time(context->total_cycles/average_times, 10);
    if (context->num_enters != context->num_exits) {
        mon_out("*");
    }
    mon_out("\n");

    mon_out("   Stolen total   %'10u cycles ", context->total_stolen_cycles);
    print_cycle_time(context->total_stolen_cycles, 10);
    mon_out("\n");

    if(context->total_stolen_cycles_self > 0) {
        profiling_counter_t total_with_stolen = context->total_cycles + context->total_stolen_cycles;

        mon_out("   Stolen self    %'10u cycles ", context->total_stolen_cycles_self);
        print_cycle_time(context->total_stolen_cycles_self, 10);
        mon_out("\n");

        mon_out("   Avg inc stolen %'10.0f cycles ", total_with_stolen/average_times);
        print_cycle_time(total_with_stolen/average_times, 10);
        if (context->num_enters != context->num_exits) {
            mon_out("*");
        }
        mon_out("\n");
    }

    if (context->num_enters != context->num_exits) {
        mon_out("             * Averages are not accurate since enters differs from exits.\n");
    }

    mon_out("\n");
    mon_out("   Memory Banking Config%s:", context->next_mem_config ? "s" : "");
    c = context;
    while (c) {
        num_memory_map_configs++;
        mon_out(" %d", c->memory_bank_config);
        c = c->next_mem_config;
    }

    if (is_merged(context)) {
        /* merged multi-config aggregate */

        print_contexts = false;
        mon_out("\n   Note: Disassembly represents multiple memory configs.");
        num_memory_map_configs = 1; /* suppress bank information output */

        /* Note: this should only be performed on aggregate contexts */
        merge_all_memory_configs(context); /* this is destructive */
    }

    if (print_contexts && context->child == NULL) {
        /* don't print context column if no children */
        print_contexts = false;
    }

    mon_out("\n\n");

    addr_column = 44;
    context_column = 7;

    if (num_memory_map_configs > 1) {
        addr_column += 5;
    }

    mon_out("       Cycles      %%         Times OPC Branch%%");
    if (num_memory_map_configs > 1) mon_out(" Bank");
    mon_out(" Address  Disassembly\n");

    mon_out("------------- ------ ------------- --- -------");
    if (num_memory_map_configs > 1) mon_out(" ----");
    mon_out(" -------  -------------------------\n");

    for (i = 0; i < 256; i++) {
        bool any_access_to_page = false;
        c = context;
        while (c) { /* loop over different memory bank configs for each page */
            if (c->page[i]) {
                any_access_to_page = true;
                break;
            }
            c = c->next_mem_config;
        }

        if (any_access_to_page) {
            for (j = 0; j < 256; j++) {
                uint16_t addr = i << 8 | j;
                bool printed_instruction_at_addr = false;

                c = context;
                while (c) { /* loop over different memory bank configs for each page */
                    profiling_page_t *page = c->page[i];
                    if (page && (page->data[j].num_cycles > 0 || page->data[j].touched)) {
                        profiling_counter_t total_cycles = page->data[j].num_cycles;
                        unsigned opc_size;
                        uint16_t dest_addr;
                        profiling_context_t *subcontext = NULL;

                        if (addr != next_addr && next_addr != 0) {
                            mon_out("%*s...\n", addr_column, "");
                        }

                        /* note: using main 'context', not 'c' here */
                        if (context->child) {
                            profiling_context_t *child = context->child;
                            do {
                                /* -2 to get to the start of the JSR instruction */
                                if (child->pc_src - 2 == addr &&
                                    child->memory_bank_config == c->memory_bank_config) {
                                    total_cycles += child->total_cycles;
                                    subcontext = child;
                                }
                                child = child->next;
                            } while (child != context->child);
                        }

                        mon_out("%'13u %5.1f%% %'13u",
                                total_cycles,
                                100.0 * total_cycles / context->total_cycles,
                                page->data[j].num_samples);

                        /* Output instruction cycle timings without decimal
                         * point only when average is exactly an integer */
                        if (page->data[j].num_cycles % page->data[j].num_samples != 0) {
                            mon_out(" %3.1f", (double)page->data[j].num_cycles / page->data[j].num_samples);
                        } else {
                            mon_out(" %-3u", page->data[j].num_cycles / page->data[j].num_samples);
                        }

                        if (print_contexts && subcontext) {
                            print_context_id(subcontext, context_column);
                        } else if (is_branch_instruction(c->memory_bank_config, addr, &opc_size, &dest_addr)) {
                            uint16_t next_inst = addr + opc_size;

                            /* 6502 specific:
                             * - a skipped branch takes 2 cycles
                             * - a taken branch takes 3 cycles if the destination
                             *   address is in the same memory page as the
                             *   instruction directly following the branch
                             * - otherwise a branch takes 4 cycles
                             *
                             * Using this we can calculate the branch frequency.
                             */
                            double branch_frequency = (double)page->data[j].num_cycles / page->data[j].num_samples - 2;

                            if ((dest_addr & 0xff00) !=
                                (next_inst & 0xff00)) {
                                /* branch across page boundary; branch takes 4
                                 * cycles, so divide by 2 */
                                branch_frequency /= 2;
                            }

                            if ((branch_frequency > 0.90 && branch_frequency < 1) ||
                                (branch_frequency < 0.10 && branch_frequency > 0)) {
                                mon_out(" %5.1f%%%*s", branch_frequency * 100, context_column - 7, "");
                            } else {
                                mon_out("   %3.0f%%%*s", branch_frequency * 100, context_column - 7, "");
                            }
                        } else {
                            mon_out("%*s", context_column, "");
                        }

                        if (num_memory_map_configs > 1) {
                            mon_out("%5d", c->memory_bank_config);
                        }

                        if (addr == reg_pc) {
                            mon_out(">");
                        } else if (addr == context->pc_dst &&
                                   c    == context) {
                            mon_out("*");
                        } else if (printed_instruction_at_addr) {
                            mon_out("/");
                        } else {
                            mon_out(" ");
                        }

                        printed_instruction_at_addr = true;

                        if (is_interrupt(addr)) {
                            print_src(addr);
                            mon_out("\n");
                        } else {
                            opc_size = mon_disassemble_oneline(e_comp_space, c->memory_bank_config, addr);
                            next_addr = addr + opc_size;
                        }
                    }
                    c = c->next_mem_config;
                }
            }
        }
    }
}

void mon_profile_disass_context(int context_id)
{
    profiling_context_t *context;


    if (!init_profiling_data()) return;

    context = profile_context_by_id(context_id);

    if (!context) {
        mon_out("Invalid context. Use \"prof graph\" to list contexts.\n");
        return;
    }

    mon_out("\n");
    mon_out("Context ");
    print_context_name(context, 0, 20);
    mon_out("\n");

    if (context->parent) {
        mon_out("\n");
        mon_out("   Parent  ");
        print_context_name(context->parent, 0, 20);
        mon_out("\n");
    }
    mon_out("\n");

    print_disass_context(context, true /* print subcontexts */);
}

static void clear_context_pages(profiling_context_t *context) {
    int i,j;
    for (i = 0; i < 256; i++) {
        profiling_page_t *page = context->page[i];
        if (page) {
            for (j = 0; j < 256; j++) {
                if (page->data[j].num_samples > 0) {
                    page->data[j].num_cycles = 0;
                    page->data[j].touched = 1;
                }
            }
        }
    }

    if (context->next_mem_config) {
        clear_context_pages(context->next_mem_config);
    }
}

static void clear_recursively(profiling_context_t *context, uint16_t addr) {
    if (context->pc_dst == addr) {
        clear_context_pages(context);
    }

    if (context->child) {
        profiling_context_t *c = context->child;
        do {
            clear_recursively(c, addr);
            c = c->next;
        } while (c != context->child);
    }
}

void mon_profile_clear(MON_ADDR function)
{
    uint16_t addr;
    if (!init_profiling_data()) return;

    if (addr_memspace(function) == e_default_space) {
        addr = addr_location(function);
    } else {
        mon_out("Invalid address space\n");
        return;
    }

    clear_recursively(root_context, addr);
}

/* ============================================================================
 * EXPORT FUNCTIONS
 * ============================================================================ */

/* Helper to get function name for export (returns static buffer) */
static const char *get_function_name(uint16_t addr) {
    static char buf[32];
    char *name = mon_symbol_table_lookup_name(default_memspace, addr);
    if (name) {
        return name;
    }
    if (addr == 0x0000) {
        return "ROOT";
    }
    snprintf(buf, sizeof(buf), "$%04X", addr);
    return buf;
}

/* Get interrupt context string for a function by walking up parent chain.
 * Returns static buffer with format like "[IRQ]" or "[IRQ>NMI]" or empty string.
 */
static const char *get_interrupt_context(profiling_context_t *context) {
    static char buf[64];
    char *p = buf;
    const char *separators[8];  /* Max 8 nested interrupts */
    int count = 0;
    profiling_context_t *ctx;

    buf[0] = '\0';

    /* Walk up the parent chain collecting interrupt info */
    for (ctx = context; ctx != NULL && count < 8; ctx = ctx->parent) {
        if (ctx->pc_src >= 0xfffa) {
            switch (ctx->pc_src) {
                case 0xfffa: separators[count++] = "NMI"; break;
                case 0xfffc: separators[count++] = "RESET"; break;
                case 0xfffe: separators[count++] = "IRQ"; break;
            }
        }
    }

    if (count == 0) {
        return "";
    }

    /* Build string in reverse order (outermost first) */
    *p++ = '[';
    while (count > 0) {
        const char *s = separators[--count];
        while (*s) *p++ = *s++;
        if (count > 0) *p++ = '>';
    }
    *p++ = ']';
    *p = '\0';

    return buf;
}

/* ============================================================================
 * CALLGRIND EXPORT WITH PSEUDO-SOURCE
 *
 * This generates two files:
 * 1. A pseudo-source .asm file with one instruction per line
 * 2. A Callgrind file referencing the pseudo-source with line numbers
 *
 * This allows Callgrind viewers (KCachegrind, Blacksmith) to show
 * per-instruction costs with the actual disassembly visible.
 * ============================================================================ */

/* Global instruction table for building pseudo-source */
typedef struct instr_entry_s {
    uint16_t addr;
    uint32_t line_num;      /* Line number in pseudo-source file */
    profiling_counter_t total_cycles;
    profiling_counter_t total_samples;
} instr_entry_t;

static instr_entry_t *global_instrs = NULL;
static int global_instrs_count = 0;
static int global_instrs_capacity = 0;

/* Helper structure for callgrind export - call edges */
typedef struct callgrind_call_s {
    uint16_t caller_addr;   /* Address of call site (for line lookup) */
    uint16_t callee_addr;
    char callee_irq_ctx[64];
    profiling_counter_t cycles;
    profiling_counter_t count;
    struct callgrind_call_s *next;
} callgrind_call_t;

/* Helper structure for callgrind export - function data */
typedef struct callgrind_func_s {
    uint16_t addr;
    char irq_ctx[64];
    profiling_counter_t self_cycles;
    profiling_counter_t self_samples;
    profiling_counter_t total_cycles;
    profiling_counter_t num_calls;
    uint16_t *instr_addrs;  /* Array of instruction addresses in this function */
    int instr_count;
    int instr_capacity;
    callgrind_call_t *calls;
    struct callgrind_func_s *next;
} callgrind_func_t;

static callgrind_func_t *callgrind_funcs = NULL;

/* Add instruction to global table (for pseudo-source generation) */
static void add_global_instr(uint16_t addr, profiling_counter_t cycles, profiling_counter_t samples) {
    int i;

    /* Check if already exists */
    for (i = 0; i < global_instrs_count; i++) {
        if (global_instrs[i].addr == addr) {
            global_instrs[i].total_cycles += cycles;
            global_instrs[i].total_samples += samples;
            return;
        }
    }

    /* Grow array if needed */
    if (global_instrs_count >= global_instrs_capacity) {
        global_instrs_capacity = global_instrs_capacity ? global_instrs_capacity * 2 : 256;
        global_instrs = lib_realloc(global_instrs, global_instrs_capacity * sizeof(instr_entry_t));
    }

    /* Add new entry */
    global_instrs[global_instrs_count].addr = addr;
    global_instrs[global_instrs_count].line_num = 0;  /* Assigned later after sorting */
    global_instrs[global_instrs_count].total_cycles = cycles;
    global_instrs[global_instrs_count].total_samples = samples;
    global_instrs_count++;
}

/* Compare function for sorting instructions by address */
static int instr_addr_compare(const void *a, const void *b) {
    const instr_entry_t *ia = (const instr_entry_t *)a;
    const instr_entry_t *ib = (const instr_entry_t *)b;
    return (int)ia->addr - (int)ib->addr;
}

/* Binary search for instruction entry by address (array must be sorted) */
static instr_entry_t *find_instr_entry(uint16_t addr) {
    int lo = 0, hi = global_instrs_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (global_instrs[mid].addr == addr) {
            return &global_instrs[mid];
        } else if (global_instrs[mid].addr < addr) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return NULL;
}

/* Look up line number for an instruction address */
static uint32_t get_line_for_addr(uint16_t addr) {
    instr_entry_t *entry = find_instr_entry(addr);
    return entry ? entry->line_num : 0;
}

static callgrind_func_t *find_or_create_callgrind_func(uint16_t addr, const char *irq_ctx) {
    callgrind_func_t *f = callgrind_funcs;
    while (f) {
        if (f->addr == addr && strcmp(f->irq_ctx, irq_ctx) == 0) return f;
        f = f->next;
    }
    /* Create new */
    f = lib_calloc(1, sizeof(callgrind_func_t));
    f->addr = addr;
    strncpy(f->irq_ctx, irq_ctx, sizeof(f->irq_ctx) - 1);
    f->irq_ctx[sizeof(f->irq_ctx) - 1] = '\0';
    f->next = callgrind_funcs;
    callgrind_funcs = f;
    return f;
}

static void add_func_instr(callgrind_func_t *func, uint16_t addr) {
    int i;
    /* Check if already in list */
    for (i = 0; i < func->instr_count; i++) {
        if (func->instr_addrs[i] == addr) return;
    }
    /* Grow array if needed */
    if (func->instr_count >= func->instr_capacity) {
        func->instr_capacity = func->instr_capacity ? func->instr_capacity * 2 : 64;
        func->instr_addrs = lib_realloc(func->instr_addrs, func->instr_capacity * sizeof(uint16_t));
    }
    func->instr_addrs[func->instr_count++] = addr;
}

static void add_callgrind_call(callgrind_func_t *caller, uint16_t caller_site_addr,
                               uint16_t callee_addr, const char *callee_irq_ctx,
                               profiling_counter_t cycles, profiling_counter_t count) {
    callgrind_call_t *c = caller->calls;
    while (c) {
        if (c->callee_addr == callee_addr && strcmp(c->callee_irq_ctx, callee_irq_ctx) == 0) {
            c->cycles += cycles;
            c->count += count;
            return;
        }
        c = c->next;
    }
    /* Create new call entry */
    c = lib_calloc(1, sizeof(callgrind_call_t));
    c->caller_addr = caller_site_addr;
    c->callee_addr = callee_addr;
    strncpy(c->callee_irq_ctx, callee_irq_ctx, sizeof(c->callee_irq_ctx) - 1);
    c->callee_irq_ctx[sizeof(c->callee_irq_ctx) - 1] = '\0';
    c->cycles = cycles;
    c->count = count;
    c->next = caller->calls;
    caller->calls = c;
}

/* Collect all instruction data from context tree */
static void collect_callgrind_data(profiling_context_t *context,
                                    profiling_context_t *parent_context) {
    callgrind_func_t *func;
    uint16_t func_addr = context->pc_dst;
    const char *irq_ctx = get_interrupt_context(context);
    int page_idx, addr_idx;

    /* Get or create function entry */
    func = find_or_create_callgrind_func(func_addr, irq_ctx);
    func->total_cycles += context->total_cycles;
    func->num_calls += context->num_enters > 0 ? context->num_enters : 1;

    /* Collect per-instruction data */
    for (page_idx = 0; page_idx < 256; page_idx++) {
        profiling_page_t *page = context->page[page_idx];
        if (page) {
            for (addr_idx = 0; addr_idx < 256; addr_idx++) {
                if (page->data[addr_idx].touched) {
                    uint16_t instr_addr = (page_idx << 8) | addr_idx;
                    profiling_counter_t cycles = page->data[addr_idx].num_cycles;
                    profiling_counter_t samples = page->data[addr_idx].num_samples;

                    /* Add to global instruction table */
                    add_global_instr(instr_addr, cycles, samples);

                    /* Track which instructions belong to this function */
                    add_func_instr(func, instr_addr);

                    /* Accumulate self costs */
                    func->self_cycles += cycles;
                    func->self_samples += samples;
                }
            }
        }
    }

    /* Add call from parent (if not root) */
    if (parent_context != NULL) {
        const char *parent_irq_ctx = get_interrupt_context(parent_context);
        callgrind_func_t *parent = find_or_create_callgrind_func(parent_context->pc_dst, parent_irq_ctx);
        /* Use pc_src as the call site address */
        add_callgrind_call(parent, context->pc_src, func_addr, irq_ctx,
                          context->total_cycles,
                          context->num_enters > 0 ? context->num_enters : 1);
    }

    /* Process children */
    if (context->child) {
        profiling_context_t *c = context->child;
        do {
            collect_callgrind_data(c, context);
            c = c->next;
        } while (c != context->child);
    }
}

static void free_callgrind_data(void) {
    callgrind_func_t *f = callgrind_funcs;
    while (f) {
        callgrind_func_t *next_f = f->next;
        callgrind_call_t *c = f->calls;
        while (c) {
            callgrind_call_t *next_c = c->next;
            lib_free(c);
            c = next_c;
        }
        if (f->instr_addrs) {
            lib_free(f->instr_addrs);
        }
        lib_free(f);
        f = next_f;
    }
    callgrind_funcs = NULL;

    if (global_instrs) {
        lib_free(global_instrs);
        global_instrs = NULL;
    }
    global_instrs_count = 0;
    global_instrs_capacity = 0;
}

/* Build function name with interrupt context if present */
static const char *get_callgrind_func_name(uint16_t addr, const char *irq_ctx) {
    static char buf[128];
    const char *name = get_function_name(addr);

    if (irq_ctx[0] != '\0') {
        snprintf(buf, sizeof(buf), "%s %s", name, irq_ctx);
        return buf;
    }
    return name;
}

/* Generate pseudo-source filename from callgrind filename */
static void get_asm_filename(const char *callgrind_file, char *asm_file, size_t asm_file_size) {
    const char *dot;
    size_t base_len;

    dot = strrchr(callgrind_file, '.');
    if (dot) {
        base_len = dot - callgrind_file;
        if (base_len >= asm_file_size - 5) {
            base_len = asm_file_size - 5;
        }
        strncpy(asm_file, callgrind_file, base_len);
        asm_file[base_len] = '\0';
    } else {
        strncpy(asm_file, callgrind_file, asm_file_size - 5);
        asm_file[asm_file_size - 5] = '\0';
    }
    strcat(asm_file, ".asm");
}

/* Get just the basename of a path */
static const char *get_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    const char *last = slash > backslash ? slash : backslash;
    return last ? last + 1 : path;
}

/* Write pseudo-source file */
static int write_pseudo_source(const char *asm_filename) {
    FILE *fp;
    int i;
    uint32_t line_num = 1;

    fp = fopen(asm_filename, MODE_WRITE_TEXT);
    if (!fp) {
        mon_out("Error: Cannot open file '%s' for writing.\n", asm_filename);
        return 0;
    }

    /* Sort instructions by address */
    qsort(global_instrs, global_instrs_count, sizeof(instr_entry_t), instr_addr_compare);

    /* Assign line numbers and write pseudo-source */
    for (i = 0; i < global_instrs_count; i++) {
        uint16_t addr = global_instrs[i].addr;
        uint8_t op, p1, p2;
        unsigned opc_size;
        const char *disasm;

        /* Assign line number */
        global_instrs[i].line_num = line_num;

        /* Get opcode bytes */
        op = mon_get_mem_val(default_memspace, addr);
        p1 = mon_get_mem_val(default_memspace, (uint16_t)(addr + 1));
        p2 = mon_get_mem_val(default_memspace, (uint16_t)(addr + 2));

        /* Get disassembly */
        disasm = mon_disassemble_to_string_ex(default_memspace, addr, op, p1, p2, 0, 1, &opc_size);

        /* Write line: "line  $ADDR: XX XX XX  DISASSEMBLY" */
        /* Format bytes based on instruction size */
        if (opc_size == 1) {
            fprintf(fp, "%6u  $%04X: %02X        %s\n", line_num, addr, op, disasm);
        } else if (opc_size == 2) {
            fprintf(fp, "%6u  $%04X: %02X %02X     %s\n", line_num, addr, op, p1, disasm);
        } else {
            fprintf(fp, "%6u  $%04X: %02X %02X %02X  %s\n", line_num, addr, op, p1, p2, disasm);
        }

        line_num++;
    }

    fclose(fp);
    return 1;
}

/* Compare function for sorting instruction addresses */
static int uint16_compare(const void *a, const void *b) {
    return (int)(*(const uint16_t *)a) - (int)(*(const uint16_t *)b);
}

void mon_profile_export(const char *filename)
{
    FILE *fp;
    callgrind_func_t *f;
    int func_count = 0;
    char asm_filename[512];
    const char *asm_basename;
    int i;
    profiling_counter_t total_samples = 0;

    if (!init_profiling_data()) return;

    /* Generate .asm filename */
    get_asm_filename(filename, asm_filename, sizeof(asm_filename));
    asm_basename = get_basename(asm_filename);

    /* Initialize global instruction table */
    global_instrs = NULL;
    global_instrs_count = 0;
    global_instrs_capacity = 0;

    /* Collect data from context tree */
    callgrind_funcs = NULL;
    collect_callgrind_data(root_context, NULL);

    /* Calculate total samples */
    for (i = 0; i < global_instrs_count; i++) {
        total_samples += global_instrs[i].total_samples;
    }

    /* Write pseudo-source file */
    if (!write_pseudo_source(asm_filename)) {
        free_callgrind_data();
        return;
    }

    /* Open Callgrind file */
    fp = fopen(filename, MODE_WRITE_TEXT);
    if (!fp) {
        mon_out("Error: Cannot open file '%s' for writing.\n", filename);
        free_callgrind_data();
        return;
    }

    /* Write Callgrind header */
    fprintf(fp, "# callgrind format\n");
    fprintf(fp, "version: 1\n");
    fprintf(fp, "creator: VICE Profiler\n");
    fprintf(fp, "pid: 1\n");
    fprintf(fp, "cmd: vice\n\n");

    fprintf(fp, "positions: line\n");
    fprintf(fp, "events: Ir Cy\n\n");

    fprintf(fp, "# CPU: 6502 @ %ld Hz\n", machine_get_cycles_per_second());
    fprintf(fp, "# Ir = Instruction executions (samples)\n");
    fprintf(fp, "# Cy = CPU cycles\n");
    fprintf(fp, "summary: %u %u\n\n", total_samples, root_context->total_cycles);

    /* Write function data */
    for (f = callgrind_funcs; f; f = f->next) {
        callgrind_call_t *c;
        const char *func_name = get_callgrind_func_name(f->addr, f->irq_ctx);

        /* Function definition - reference pseudo-source file */
        fprintf(fp, "fl=%s\n", asm_basename);
        fprintf(fp, "fn=%s\n", func_name);

        /* Sort instructions by address for consistent output */
        if (f->instr_count > 0) {
            qsort(f->instr_addrs, f->instr_count, sizeof(uint16_t), uint16_compare);
        }

        /* Output per-instruction costs using line numbers */
        for (i = 0; i < f->instr_count; i++) {
            instr_entry_t *entry = find_instr_entry(f->instr_addrs[i]);
            if (entry && entry->line_num > 0) {
                fprintf(fp, "%u %u %u\n", entry->line_num,
                        entry->total_samples, entry->total_cycles);
            }
        }

        /* Calls to other functions */
        for (c = f->calls; c; c = c->next) {
            const char *callee_name = get_callgrind_func_name(c->callee_addr, c->callee_irq_ctx);
            uint32_t caller_line = get_line_for_addr(c->caller_addr);
            uint32_t callee_line = get_line_for_addr(c->callee_addr);

            /* If we don't have a line for the call site, use function entry */
            if (caller_line == 0) {
                caller_line = get_line_for_addr(f->addr);
            }
            if (callee_line == 0) {
                callee_line = 1;
            }

            fprintf(fp, "cfl=%s\n", asm_basename);
            fprintf(fp, "cfn=%s\n", callee_name);
            fprintf(fp, "calls=%u %u\n", c->count, callee_line);
            /* Cost attributed to the call site: samples=count, cycles */
            fprintf(fp, "%u %u %u\n", caller_line > 0 ? caller_line : 1, c->count, c->cycles);
        }

        fprintf(fp, "\n");
        func_count++;
    }

    /* Write totals */
    fprintf(fp, "totals: %u %u\n", total_samples, root_context->total_cycles);

    fclose(fp);

    mon_out("Exported %d functions to '%s'\n", func_count, filename);
    mon_out("Pseudo-source: %s (%d instructions)\n", asm_filename, global_instrs_count);
    mon_out("Open with: kcachegrind %s\n", filename);

    free_callgrind_data();
}

