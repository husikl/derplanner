//
// Copyright (c) 2015 Alexander Shafranov shafranov@gmail.com
//
// This software is provided 'as-is', without any express or implied
// warranty.  In no event will the authors be held liable for any damages
// arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//
// 1. The origin of this software must not be misrepresented; you must not
// claim that you wrote the original software. If you use this software
// in a product, an acknowledgment in the product documentation would be
// appreciated but is not required.
// 2. Altered source versions must be plainly marked as such, and must not be
// misrepresented as being the original software.
// 3. This notice may not be removed or altered from any source distribution.
//

#include <algorithm> // std::sort

#include "derplanner/compiler/io.h"
#include "derplanner/compiler/array.h"
#include "derplanner/compiler/lexer.h"
#include "derplanner/compiler/id_table.h"
#include "derplanner/compiler/string_buffer.h"
#include "derplanner/compiler/signature_table.h"
#include "derplanner/compiler/ast.h"
#include "derplanner/compiler/codegen.h"

#include "derplanner/runtime/database.h" // murmur2_32

using namespace plnnrc;

void plnnrc::init(Codegen& state, ast::Root* tree, Memory_Stack* scratch)
{
    state.tree = tree;
    state.scratch = scratch;
}

void plnnrc::generate_header(Codegen& state, const char* header_guard, Writer* output)
{
    init(state.fmtr, "  ", "\n", output);
    ast::Root* tree = state.tree;
    ast::Domain* domain = tree->domain;
    Formatter& fmtr = state.fmtr;

    Token_Value domain_name = domain->name;
    writeln(fmtr, "// generated by derplanner [http://www.github.com/alexshafranov/derplanner]");
    writeln(fmtr, "#ifndef %s", header_guard);
    writeln(fmtr, "#define %s", header_guard);
    writeln(fmtr, "#pragma once");
    newline(fmtr);
    writeln(fmtr, "#include \"derplanner/runtime/types.h\"");
    newline(fmtr);
    writeln(fmtr, "#ifndef PLNNR_DOMAIN_API");
    writeln(fmtr, "#define PLNNR_DOMAIN_API");
    writeln(fmtr, "#endif");
    newline(fmtr);
    writeln(fmtr, "extern \"C\" PLNNR_DOMAIN_API void %n_init_domain_info();", domain_name);
    writeln(fmtr, "extern \"C\" PLNNR_DOMAIN_API const plnnr::Domain_Info* %n_get_domain_info();", domain_name);
    newline(fmtr);
    writeln(fmtr, "#endif");
    flush(fmtr);
}

static const char* s_runtime_type_tag[] =
{
    #define PLNNR_TYPE(TAG, TYPE) #TAG,
    #include "derplanner/runtime/type_tags.inl"
    #undef PLNNR_TYPE
};

static const char* s_runtime_type_enum[] =
{
    #define PLNNR_TYPE(TAG, TYPE) "Type_" #TAG,
    #include "derplanner/runtime/type_tags.inl"
    #undef PLNNR_TYPE
};

static const char* s_runtime_type_name[] =
{
    #define PLNNR_TYPE(TAG, TYPE) #TYPE,
    #include "derplanner/runtime/type_tags.inl"
    #undef PLNNR_TYPE
};

static inline const char* get_runtime_type_tag(Token_Type token_type)
{
    plnnrc_assert(token_type >= (Token_Type)Token_Group_Type_First);
    return s_runtime_type_tag[token_type - Token_Group_Type_First];
}

static inline const char* get_runtime_type_enum(Token_Type token_type)
{
    plnnrc_assert(token_type >= (Token_Type)Token_Group_Type_First);
    return s_runtime_type_enum[token_type - Token_Group_Type_First];
}

static inline const char* get_runtime_type_name(Token_Type token_type)
{
    plnnrc_assert(token_type >= (Token_Type)Token_Group_Type_First);
    return s_runtime_type_name[token_type - Token_Group_Type_First];
}

static inline Signature get_composite_task_signature(Codegen& state, ast::Task* task)
{
    ast::Primitive* prim = state.tree->primitive;
    ast::Domain* domain = state.tree->domain;
    uint32_t task_idx = index_of(domain->tasks, task);
    return get_sparse(state.task_and_pout_sigs, size(prim->tasks) + task_idx);
}

static inline Signature get_precond_output_signature(Codegen& state, ast::Case* case_)
{
    ast::Primitive* prim = state.tree->primitive;
    ast::Domain* domain = state.tree->domain;
    uint32_t case_idx = index_of(state.tree->cases, case_);
    return get_sparse(state.task_and_pout_sigs, size(prim->tasks) + size(domain->tasks) + case_idx);
}

static inline Signature get_precond_input_signature(Codegen& state, ast::Case* case_)
{
    uint32_t case_idx = index_of(state.tree->cases, case_);
    return get_sparse(state.pin_sigs, case_idx);
}

static void build_signatures(Codegen& state)
{
    ast::Root* tree = state.tree;
    ast::Primitive* prim = tree->primitive;
    ast::Domain* domain = tree->domain;

    // primitive task signatures.
    for (uint32_t task_idx = 0; task_idx < size(prim->tasks); ++task_idx)
    {
        ast::Fact* task = prim->tasks[task_idx];

        begin_signature(state.task_and_pout_sigs);
        for (uint32_t param_idx = 0; param_idx < size(task->params); ++param_idx)
        {
            add_param(state.task_and_pout_sigs, task->params[param_idx]->data_type);
        }
        end_signature(state.task_and_pout_sigs);
    }

    // compisite task signatures.
    for (uint32_t task_idx = 0; task_idx < size(domain->tasks); ++task_idx)
    {
        ast::Task* task = domain->tasks[task_idx];

        begin_signature(state.task_and_pout_sigs);
        for (uint32_t param_idx = 0; param_idx < size(task->params); ++param_idx)
        {
            add_param(state.task_and_pout_sigs, task->params[param_idx]->data_type);
        }
        end_signature(state.task_and_pout_sigs);
    }

    // precondition output
    for (uint32_t case_idx = 0; case_idx < size(tree->cases); ++case_idx)
    {
        ast::Case* case_ = tree->cases[case_idx];

        begin_signature(state.task_and_pout_sigs);
        for (uint32_t var_idx = 0; var_idx < size(case_->precond_vars); ++var_idx)
        {
            ast::Var* var = case_->precond_vars[var_idx];
            // skip bound vars
            if (var->definition != 0) { continue; }
            // save types of "output" vars
            var->output_index = add_param(state.task_and_pout_sigs, var->data_type);
        }
        end_signature(state.task_and_pout_sigs);
    }

    // precondition inputs (stored in a separate table)
    for (uint32_t case_idx = 0; case_idx < size(tree->cases); ++case_idx)
    {
        ast::Case* case_ = tree->cases[case_idx];
        ast::Task* task = case_->task;

        begin_signature(state.pin_sigs);
        for (uint32_t param_idx = 0; param_idx < size(task->params); ++param_idx)
        {
            ast::Param* param = task->params[param_idx];
            ast::Var* var = get(case_->precond_var_lookup, param->name);
            if (!var) { continue; }
            var->input_index = add_param(state.pin_sigs, var->data_type);
        }
        end_signature(state.pin_sigs);
    }
}

static void build_expand_names(String_Buffer& expand_names, ast::Domain* domain)
{
    for (uint32_t task_idx = 0; task_idx < plnnrc::size(domain->tasks); ++task_idx)
    {
        ast::Task* task = domain->tasks[task_idx];
        for (uint32_t case_idx = 0; case_idx < plnnrc::size(task->cases); ++case_idx)
        {
            begin_string(expand_names);
            write(expand_names.buffer, "%n_case_%d", task->name, case_idx);
            end_string(expand_names);
        }
    }
}

static bool all_unique(Array<uint32_t>& hashes)
{
    std::sort(&hashes[0], &hashes[0] + size(hashes));
    for (uint32_t i = 1; i < size(hashes); ++i)
    {
        uint32_t prev = hashes[i - 1];
        uint32_t curr = hashes[i + 0];
        if (curr == prev)
        {
            return false;
        }
    }

    return true;
}

static void build_hashes(const Array<Token_Value>& names, Array<uint32_t>& out_hashes, uint32_t& out_seed)
{
    resize(out_hashes, size(names));
    const uint32_t max_seed = 1000;

    for (uint32_t test_seed = 0; test_seed < max_seed; ++test_seed)
    {
        for (uint32_t i = 0; i < size(names); ++i)
        {
            out_hashes[i] = plnnr::murmur2_32(names[i].str, names[i].length, test_seed);
        }

        if (all_unique(out_hashes))
        {
            out_seed = test_seed;

            // rebuild hashes as the order of out_hashes was destroyed by sorting in `all_unique`.
            for (uint32_t i = 0; i < size(names); ++i)
            {
                out_hashes[i] = plnnr::murmur2_32(names[i].str, names[i].length, test_seed);
            }

            break;
        }
    }
}

static void generate_precondition(Codegen& state, ast::Case* case_, uint32_t case_idx, uint32_t input_idx, Signature input_sig, Signature output_sig, Formatter& fmtr);

static void generate_expansion(Codegen& state, ast::Case* case_, uint32_t case_idx, Formatter& fmtr);

void plnnrc::generate_source(Codegen& state, const char* domain_header, Writer* output)
{
    Memory_Stack_Scope scratch_scope(state.scratch);

    init(state.fmtr, "  ", "\n", output);
    ast::Root* tree = state.tree;
    ast::World* world = tree->world;
    ast::Primitive* prim = tree->primitive;
    ast::Domain* domain = tree->domain;
    Formatter& fmtr = state.fmtr;

    init(state.expand_names, state.scratch, 64, 4096);
    init(state.task_and_pout_sigs, state.scratch, size(prim->tasks) + size(domain->tasks) + size(tree->cases));
    init(state.pin_sigs, state.scratch, size(tree->cases));

    // includes & pragmas
    {
        writeln(fmtr, "// generated by derplanner [http://www.github.com/alexshafranov/derplanner]");
        writeln(fmtr, "#include \"derplanner/runtime/domain_support.h\"");
        writeln(fmtr, "#include \"%s\"", domain_header);
        newline(fmtr);
        writeln(fmtr, "using namespace plnnr;");
        newline(fmtr);
        writeln(fmtr, "#ifdef __GNUC__");
        writeln(fmtr, "#pragma GCC diagnostic ignored \"-Wunused-parameter\"");
        writeln(fmtr, "#pragma GCC diagnostic ignored \"-Wunused-variable\"");
        writeln(fmtr, "#endif");
        newline(fmtr);
        writeln(fmtr, "#ifdef _MSC_VER");
        writeln(fmtr, "#pragma warning(disable: 4100) // unreferenced formal parameter");
        writeln(fmtr, "#pragma warning(disable: 4189) // local variable is initialized but not referenced");
        writeln(fmtr, "#endif");
        newline(fmtr);
    }

    build_expand_names(state.expand_names, domain);

    // expand forward declarations
    {
        for (uint32_t case_idx = 0; case_idx < size(state.expand_names); ++case_idx)
        {
            Token_Value name = get(state.expand_names, case_idx);
            writeln(fmtr, "static bool %n(Planning_State*, Expansion_Frame*, Fact_Database*);", name);
        }
        newline(fmtr);
    }

    // s_task_expands
    {
        writeln(fmtr, "static Composite_Task_Expand* s_task_expands[] = {");
        for (uint32_t task_idx = 0; task_idx < size(domain->tasks); ++task_idx)
        {
            ast::Task* task = domain->tasks[task_idx];
            Indent_Scope s(fmtr);
            writeln(fmtr, "%n_case_0,", task->name);
        }
        writeln(fmtr, "};");
        newline(fmtr);
    }

    // s_fact_names
    {
        writeln(fmtr, "static const char* s_fact_names[] = {");
        for (uint32_t fact_idx = 0; fact_idx < size(world->facts); ++fact_idx)
        {
            Indent_Scope s(fmtr);
            ast::Fact* fact = world->facts[fact_idx];
            writeln(fmtr, "\"%n\",", fact->name);
        }
        writeln(fmtr, " };");
        newline(fmtr);
    }

    // s_task_names
    {
        writeln(fmtr, "static const char* s_task_names[] = {");
        // primitive tasks go first.
        for (uint32_t prim_idx = 0; prim_idx < size(prim->tasks); ++prim_idx)
        {
            Indent_Scope s(fmtr);
            ast::Fact* task = prim->tasks[prim_idx];
            writeln(fmtr, "\"%n\",", task->name);
        }
        // composite tasks after.
        for (uint32_t task_idx = 0; task_idx < size(domain->tasks); ++task_idx)
        {
            Indent_Scope s(fmtr);
            ast::Task* task = domain->tasks[task_idx];
            writeln(fmtr, "\"%n\",", task->name);
        }
        writeln(fmtr, " };");
        newline(fmtr);
    }

    // s_fact_types
    {
        writeln(fmtr, "static Fact_Type s_fact_types[] = {");
        for (uint32_t fact_idx = 0; fact_idx < size(world->facts); ++fact_idx)
        {
            ast::Fact* fact = world->facts[fact_idx];
            uint32_t num_params = size(fact->params);
            Indent_Scope s(fmtr);
            write(fmtr, "%i{ %d, {", num_params);
            for (uint32_t param_idx = 0; param_idx < num_params; ++param_idx)
            {
                const char* type_name = get_runtime_type_enum(fact->params[param_idx]->data_type);
                write(fmtr, "%s, ", type_name);
            }
            write(fmtr, "} },");
            newline(fmtr);
        }
        writeln(fmtr, "};");
        newline(fmtr);
    }

    build_signatures(state);

    // s_layout_types, s_layout_offsets
    if (size(state.task_and_pout_sigs.types) > 0)
    {
        writeln(fmtr, "static Type s_layout_types[] = {");
        for (uint32_t type_idx = 0; type_idx < size(state.task_and_pout_sigs.types); ++type_idx)
        {
            Token_Type type = state.task_and_pout_sigs.types[type_idx];
            Indent_Scope s(fmtr);
            writeln(fmtr, "%s,", get_runtime_type_enum(type));
        }
        writeln(fmtr, "};");
        newline(fmtr);

        writeln(fmtr, "static size_t s_layout_offsets[%d];", size(state.task_and_pout_sigs.types));
        newline(fmtr);
    }

    // s_task_parameters & s_precond_output.
    {
        struct Format_Param_Layout
        {
            void operator()(Formatter& fmtr, Signature_Table& signatures, uint32_t sig_idx)
            {
                Signature sig = get_sparse(signatures, sig_idx);

                Indent_Scope s(fmtr);
                if (sig.length > 0)
                {
                    writeln(fmtr, "{ %d, s_layout_types + %d, 0, s_layout_offsets + 0 },", sig.length, sig.offset);
                }
                else
                {
                    writeln(fmtr, "{ 0, 0, 0, 0 },");
                }
            }
        };

        const uint32_t num_tasks = size(prim->tasks) + size(domain->tasks);

        writeln(fmtr, "static Param_Layout s_task_parameters[] = {");
        for (uint32_t sig_idx = 0; sig_idx < num_tasks; ++sig_idx)
        {
            Format_Param_Layout()(fmtr, state.task_and_pout_sigs, sig_idx);
        }
        writeln(fmtr, "};");
        newline(fmtr);

        writeln(fmtr, "static Param_Layout s_precond_output[] = {");
        for (uint32_t sig_idx = num_tasks; sig_idx < size(state.task_and_pout_sigs.remap); ++sig_idx)
        {
            Format_Param_Layout()(fmtr, state.task_and_pout_sigs, sig_idx);
        }
        writeln(fmtr, "};");
        newline(fmtr);
    }

    // s_num_cases
    {
        writeln(fmtr, "static uint32_t s_num_cases[] = {");
        for (uint32_t task_idx = 0; task_idx < size(domain->tasks); ++task_idx)
        {
            ast::Task* task = domain->tasks[task_idx];
            Indent_Scope s(fmtr);
            writeln(fmtr, "%d, ", size(task->cases));
        }
        writeln(fmtr, "};");
        newline(fmtr);
    }

    // s_size_hints
    {
        writeln(fmtr, "static uint32_t s_size_hints[] = {");
        for (uint32_t fact_idx = 0; fact_idx < size(world->facts); ++fact_idx)
        {
            Indent_Scope s(fmtr);
            writeln(fmtr, "0, ");
        }
        writeln(fmtr, "};");
        newline(fmtr);
    }

    uint32_t fact_names_hash_seed = 0;
    // s_fact_name_hashes
    {
        Memory_Stack_Scope scratch_scope(state.scratch);

        Array<Token_Value> fact_names;
        init(fact_names, state.scratch, size(world->facts));
        Array<uint32_t> fact_name_hashes;
        init(fact_name_hashes, state.scratch, size(world->facts));

        for (uint32_t fact_idx = 0; fact_idx < size(world->facts); ++fact_idx)
        {
            ast::Fact* fact = world->facts[fact_idx];
            push_back(fact_names, fact->name);
        }

        build_hashes(fact_names, fact_name_hashes, fact_names_hash_seed);

        writeln(fmtr, "static uint32_t s_fact_name_hashes[] = {");
        for (uint32_t fact_idx = 0; fact_idx < size(world->facts); ++fact_idx)
        {
            Indent_Scope s(fmtr);
            writeln(fmtr, "%u, ", fact_name_hashes[fact_idx]);
        }
        writeln(fmtr, "};");
        newline(fmtr);
    }

    uint32_t task_names_hash_seed = 0;
    // s_task_name_hashes
    {
        Memory_Stack_Scope scratch_scope(state.scratch);

        const uint32_t num_tasks = size(prim->tasks) + size(domain->tasks);
        Array<Token_Value> task_names;
        init(task_names, state.scratch, num_tasks);
        Array<uint32_t> task_name_hashes;
        init(task_name_hashes, state.scratch, num_tasks);

        // primtives go first
        for (uint32_t prim_idx = 0; prim_idx < size(prim->tasks); ++prim_idx)
        {
            push_back(task_names, prim->tasks[prim_idx]->name);
        }
        // composite next
        for (uint32_t task_idx = 0; task_idx < size(domain->tasks); ++task_idx)
        {
            push_back(task_names, domain->tasks[task_idx]->name);
        }

        build_hashes(task_names, task_name_hashes, task_names_hash_seed);

        writeln(fmtr, "static uint32_t s_task_name_hashes[] = {");
        for (uint32_t hash_idx = 0; hash_idx < size(task_name_hashes); ++hash_idx)
        {
            Indent_Scope s(fmtr);
            writeln(fmtr, "%u, ", task_name_hashes[hash_idx]);
        }
        writeln(fmtr, "};");
        newline(fmtr);
    }

    // s_domain_info
    {
        const uint32_t num_tasks = size(prim->tasks) + size(domain->tasks);
        const uint32_t num_primitive = size(prim->tasks);
        const uint32_t num_composite = size(domain->tasks);
        writeln(fmtr, "static Domain_Info s_domain_info = {");
        {
            Indent_Scope s(fmtr);
            // task_info
            writeln(fmtr, "{ %d, %d, %d, s_num_cases, %d, s_task_name_hashes, s_task_names, s_task_parameters, s_precond_output, s_task_expands },",
                num_tasks, num_primitive, num_composite, task_names_hash_seed);
            // database_req
            writeln(fmtr, "{ %d, %d, s_size_hints, s_fact_types, s_fact_name_hashes, s_fact_names },", size(world->facts), fact_names_hash_seed);
        }
        writeln(fmtr, "};");
        newline(fmtr);
    }

    // init_domain_info & get_domain_info
    {
        writeln(fmtr, "void %n_init_domain_info()", domain->name);
        writeln(fmtr, "{");
        {
            Indent_Scope s(fmtr);
            writeln(fmtr, "for (size_t i = 0; i < plnnr_static_array_size(s_task_parameters); ++i) {");
            {
                Indent_Scope s(fmtr);
                writeln(fmtr, "compute_offsets_and_size(s_task_parameters[i]);");
            }
            writeln(fmtr, "}");
            newline(fmtr);

            writeln(fmtr, "for (size_t i = 0; i < plnnr_static_array_size(s_precond_output); ++i) {");
            {
                Indent_Scope s(fmtr);
                writeln(fmtr, "compute_offsets_and_size(s_precond_output[i]);");
            }
            writeln(fmtr, "}");
        }
        writeln(fmtr, "}");
        newline(fmtr);

        writeln(fmtr, "const Domain_Info* %n_get_domain_info() { return &s_domain_info; }", domain->name);
        newline(fmtr);
    }

    // precondition input structs
    {
        for (uint32_t input_idx = 0; input_idx < size_dense(state.pin_sigs); ++input_idx)
        {
            Signature input_sig = get_dense(state.pin_sigs, input_idx);
            if (input_sig.length == 0) { continue; }

            writeln(fmtr, "struct input_%d {", input_idx);
            for (uint32_t param_idx = 0; param_idx < input_sig.length; ++param_idx)
            {
                Indent_Scope s(fmtr);
                Token_Type param_type = input_sig.types[param_idx];
                const char* type_name = get_runtime_type_name(param_type);
                writeln(fmtr, "%s _%d;", type_name, param_idx);
            }
            writeln(fmtr, "};");
            newline(fmtr);
        }
    }

    // precondition iterators
    for (uint32_t case_idx = 0; case_idx < size(tree->cases); ++case_idx)
    {
        ast::Case* case_ = tree->cases[case_idx];
        Signature input_sig = get_sparse(state.pin_sigs, case_idx);
        Signature output_sig = get_precond_output_signature(state, case_);
        uint32_t input_idx = get_dense_index(state.pin_sigs, case_idx);
        generate_precondition(state, case_, case_idx, input_idx, input_sig, output_sig, fmtr);
    }

    // case expansions
    for (uint32_t case_idx = 0; case_idx < size(tree->cases); ++case_idx)
    {
        ast::Case* case_ = tree->cases[case_idx];
        generate_expansion(state, case_, case_idx, fmtr);
    }

    flush(fmtr);
}

static void generate_literal_chain(Codegen& state, ast::Case* case_, ast::Expr* node, uint32_t& handle_id, uint32_t yield_id, Formatter& fmtr);

static void generate_precondition(Codegen& state, ast::Case* case_, uint32_t case_idx, uint32_t input_idx, Signature input_sig, Signature output_sig, Formatter& fmtr)
{
    if (input_sig.length > 0)
    {
        writeln(fmtr, "static bool p%d_next(Planning_State* state, Expansion_Frame* frame, Fact_Database* db, const input_%d* args)", case_idx, input_idx);
    }
    else
    {
        writeln(fmtr, "static bool p%d_next(Planning_State* state, Expansion_Frame* frame, Fact_Database* db)", case_idx);
    }

    ast::Expr* precond = case_->precond;
    plnnrc_assert(is_Or(precond));

    // trivial precondition expression.
    if (!precond->child)
    {
        writeln(fmtr, "{");
        {
            Indent_Scope s(fmtr);
            writeln(fmtr, "plnnr_coroutine_begin(frame, precond_label);");
            writeln(fmtr, "plnnr_coroutine_yield(frame, precond_label, 1);");
            writeln(fmtr, "plnnr_coroutine_end();");
        }
        writeln(fmtr, "}");
        newline(fmtr);
        return;
    }

    writeln(fmtr, "{");
    {
        Indent_Scope s(fmtr);
        writeln(fmtr, "Fact_Handle* handles = frame->handles;");
        writeln(fmtr, "const Param_Layout& output_layout = s_precond_output[%d];", case_idx);
        newline(fmtr);
        writeln(fmtr, "plnnr_coroutine_begin(frame, precond_label);");

        const uint32_t num_fact_handles = size(case_->precond_facts);
        if (num_fact_handles > 0)
        {
            writeln(fmtr, "handles = allocate_precond_handles(state, frame, %d);", num_fact_handles);
        }

        if (output_sig.length > 0)
        {
            writeln(fmtr, "allocate_precond_output(state, frame, output_layout);");
        }
        newline(fmtr);

        uint32_t handle_id = 0;
        uint32_t yield_id = 1;
        for (ast::Expr* conjunct = precond->child; conjunct != 0; conjunct = conjunct->next_sibling)
        {
            if (is_And(conjunct))
            {
                generate_literal_chain(state, case_, conjunct->child, handle_id, yield_id, fmtr);
                continue;
            }

            generate_literal_chain(state, case_, conjunct, handle_id, yield_id, fmtr);
        }

        newline(fmtr);
        writeln(fmtr, "plnnr_coroutine_end();");
    }

    writeln(fmtr, "}");
    newline(fmtr);
}

static void generate_literal_chain(Codegen& state, ast::Case* case_, ast::Expr* literal, uint32_t& handle_id, uint32_t yield_id, Formatter& fmtr)
{
    ast::Func* func = is_Not(literal) ? as_Func(literal->child) : as_Func(literal);
    plnnrc_assert(func);
    ast::Fact* fact = get_fact(*state.tree, func->name);
    uint32_t fact_idx = index_of(state.tree->world->facts, fact);

    writeln(fmtr, "for (handles[%d] = first(db, %d); is_valid(db, handles[%d]); handles[%d] = next(db, handles[%d])) { // %n",
        handle_id, fact_idx, handle_id, handle_id, handle_id, fact->name);
    {
        Indent_Scope indent_scope(fmtr);

        const char* comparison_op = is_Not(literal) ? "==" : "!=";
        uint32_t fact_param_idx = 0;
        for (ast::Expr* arg = func->child; arg != 0; arg = arg->next_sibling, ++fact_param_idx)
        {
            ast::Var* var = as_Var(arg);
            plnnrc_assert(var);
            Token_Type data_type = var->data_type;
            const char* data_type_tag = get_runtime_type_tag(data_type);

            ast::Node* def = var->definition;
            // output variable -> skip
            if (!def) { continue; }

            // parameter -> use `args` struct
            if (ast::Param* param = as_Param(def))
            {
                ast::Var* first = get(case_->precond_var_lookup, param->name);
                writeln(fmtr, "if (args->_%d %s as_%s(db, handles[%d], %d)) {",
                    first->input_index, comparison_op, data_type_tag, handle_id, fact_param_idx);
                {
                    Indent_Scope s(fmtr);
                    writeln(fmtr, "continue;");
                }
                writeln(fmtr, "}");
                newline(fmtr);
                continue;
            }

            // variable -> compare handle to handle
            plnnrc_assert(false);
        }

        fact_param_idx = 0;
        uint32_t output_idx = 0;
        for (ast::Expr* arg = func->child; arg != 0; arg = arg->next_sibling, ++fact_param_idx)
        {
            ast::Var* var = as_Var(arg);
            plnnrc_assert(var);
            Token_Type data_type = var->data_type;
            const char* data_type_tag = get_runtime_type_tag(data_type);

            ast::Node* def = var->definition;
            if (def) { continue; }

            writeln(fmtr, "set_precond_output(frame, output_layout, %d, as_%s(db, handles[%d], %d));",
                handle_id + output_idx, data_type_tag, handle_id, fact_param_idx);

            ++output_idx;
        }

        ++handle_id;

        if (!literal->next_sibling)
        {
            writeln(fmtr, "plnnr_coroutine_yield(frame, precond_label, %d);", yield_id);
            ++yield_id;
        }
        else
        {
            generate_literal_chain(state, case_, literal->next_sibling, handle_id, yield_id, fmtr);
        }
    }

    writeln(fmtr, "}");
}

static void generate_arg_setters(Codegen& state, const char* set_arg_name, ast::Func* task_list_item, ast::Case* case_, uint32_t target_task_index, Formatter& fmtr)
{
    ast::Task* task = case_->task;
    uint32_t case_index = index_of(state.tree->cases, case_);
    uint32_t task_params_idx = size(state.tree->primitive->tasks) + index_of(state.tree->domain->tasks, task);

    uint32_t arg_index = 0;
    for (ast::Expr* arg_node = task_list_item->child; arg_node != 0; arg_node = arg_node->next_sibling, ++arg_index)
    {
        ast::Var* arg_var = as_Var(arg_node);
        plnnrc_assert(arg_var);

        if (ast::Var* def_var = as_Var(arg_var->definition))
        {
            const char* data_type_tag = get_runtime_type_tag(def_var->data_type);
            writeln(fmtr, "%s(state, s_task_parameters[%d], %d, as_%s(frame->precond_output, s_precond_output[%d], %d));",
                set_arg_name, target_task_index, arg_index, data_type_tag, case_index, def_var->output_index);
            continue;
        }

        if (ast::Param* def_param = as_Param(arg_var->definition))
        {
            const char* data_type_tag = get_runtime_type_tag(def_param->data_type);
            uint32_t task_param_idx = index_of(task->params, def_param);
            writeln(fmtr, "%s(state, s_task_parameters[%d], %d, as_%s(frame->arguments, s_task_parameters[%d], %d));",
                set_arg_name, target_task_index, arg_index, data_type_tag, task_params_idx, task_param_idx);
            continue;
        }

        plnnrc_assert(false);
    }
}

static void generate_expansion(Codegen& state, ast::Case* case_, uint32_t case_idx, Formatter& fmtr)
{
    ast::Task* task = case_->task;
    Token_Value name = get(state.expand_names, case_idx);
    uint32_t task_params_idx = size(state.tree->primitive->tasks) + index_of(state.tree->domain->tasks, task);
    uint32_t yield_id = 1;

    Signature task_sig = get_composite_task_signature(state, task);
    Signature precond_input_sig = get_precond_input_signature(state, case_);

    writeln(fmtr, "static bool %n(Planning_State* state, Expansion_Frame* frame, Fact_Database* db)", name);
    writeln(fmtr, "{");
    {
        Indent_Scope s(fmtr);

        // code to initialize precondition arguments struct.
        if (precond_input_sig.length > 0)
        {
            writeln(fmtr, "input_%d args;", get_dense_index(state.pin_sigs, case_idx));

            uint32_t input_idx = 0;
            for (uint32_t param_idx = 0; param_idx < size(task->params); ++param_idx)
            {
                ast::Param* param = task->params[param_idx];
                ast::Var* var = get(case_->precond_var_lookup, param->name);
                if (!var) { continue; }

                const char* data_type_tag = get_runtime_type_tag(task_sig.types[param_idx]);

                writeln(fmtr, "args._%d = as_%s(frame->arguments, s_task_parameters[%d], %d);",
                    input_idx, data_type_tag, task_params_idx, param_idx);
                ++input_idx;
            }
            newline(fmtr);
        }

        writeln(fmtr, "plnnr_coroutine_begin(frame, expand_label);");
        newline(fmtr);

        if (precond_input_sig.length > 0)
        {
            writeln(fmtr, "while (p%d_next(state, frame, db, &args)) {", case_idx);
        }
        else
        {
            writeln(fmtr, "while (p%d_next(state, frame, db)) {", case_idx);
        }

        // generate task list expansion
        {
            Indent_Scope s(fmtr);

            if (empty(case_->task_list))
            {
                writeln(fmtr, "plnnr_coroutine_yield(frame, expand_label, %d);", yield_id);
                ++yield_id;
            }

            for (uint32_t item_idx = 0; item_idx < size(case_->task_list); ++item_idx)
            {
                ast::Func* item = as_Func(case_->task_list[item_idx]);
                // adding primitive task to expansion.
                if (ast::Fact* primitive = get_primitive(*state.tree, item->name))
                {
                    uint32_t primitive_index = index_of(state.tree->primitive->tasks, primitive);
                    writeln(fmtr, "begin_task(state, %d, s_task_parameters[%d]); // %n",
                        primitive_index, primitive_index, primitive->name);

                    generate_arg_setters(state, "set_task_arg", item, case_, primitive_index, fmtr);

                    if (item == back(case_->task_list))
                    {
                        writeln(fmtr, "frame->flags |= Expansion_Frame::Flags_Expanded;");
                    }

                    writeln(fmtr, "plnnr_coroutine_yield(frame, expand_label, %d);", yield_id);
                    ++yield_id;
                    newline(fmtr);

                    continue;
                }

                // adding composite task to expansion.
                if (ast::Task* composite = get_task(*state.tree, item->name))
                {
                    uint32_t composite_index = size(state.tree->primitive->tasks) + index_of(state.tree->domain->tasks, composite);
                    writeln(fmtr, "begin_composite(state, %d, %n_case_0, s_task_parameters[%d]); // %n",
                        composite_index, composite->name, composite_index, composite->name);

                    generate_arg_setters(state, "set_composite_arg", item, case_, composite_index, fmtr);

                    if (item == back(case_->task_list))
                    {
                        writeln(fmtr, "frame->flags |= Expansion_Frame::Flags_Expanded;");
                    }

                    writeln(fmtr, "plnnr_coroutine_yield(frame, expand_label, %d);", yield_id);
                    ++yield_id;

                    if (item != back(case_->task_list))
                    {
                        writeln(fmtr, "if ((frame->flags & Expansion_Frame::Flags_Failed) != 0) { continue; }");
                    }

                    newline(fmtr);

                    continue;
                }

                plnnrc_assert(false);
            }
        }

        writeln(fmtr, "}");
        newline(fmtr);

        if (case_ != back(task->cases))
        {
            Token_Value next_expand_name = get(state.expand_names, case_idx + 1);
            writeln(fmtr, "return expand_next_case(state, frame, db, %n, s_task_parameters[%d]);",
                next_expand_name, task_params_idx);
            newline(fmtr);
        }

        writeln(fmtr, "plnnr_coroutine_end();");
    }

    writeln(fmtr, "}");
    newline(fmtr);
}
