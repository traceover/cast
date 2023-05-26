#include <stdio.h>

#include "common.h"
#include "lexer.h"
#include "ast.h"
#include "type_info.h"
#include "vendor/stb_ds.h"

const char *type_to_string(Type type)
{
    if (type == NULL) return "**NULL**";
    
    // Builtin types
    if (type == &type_info_int)             return "int";
    if (type == &type_info_float)           return "float";
    if (type == &type_info_float64)         return "float64";
    if (type == &type_info_comptime_int)    return "comptime_int";
    if (type == &type_info_comptime_float)  return "comptime_float";
    if (type == &type_info_comptime_string) return "comptime_string";
    
    // Otherwise, construct the string dynamically
    switch (type->tag) {
        case TYPE_INTEGER:
            if (type->integer.sign) {
                return tprint("s%ld", type->runtime_size * 8);
            }
            return tprint("u%ld", type->runtime_size * 8);

        case TYPE_FLOAT:  return tprint("f%ld", type->runtime_size * 8);

        case TYPE_STRING: return "string";
        case TYPE_BOOL:   return "bool";
        case TYPE_VOID:   return "void";
        case TYPE_TYPE:   return "Type";

        case TYPE_PROCEDURE: {
            Arena *old = context_arena;
            context_arena = &temporary_arena;
            String_Builder sb = {0};

            sb_append_cstr(&sb, "(");
            for (size_t i = 0; i < type->procedure.parameter_count; ++i) {
                if (i > 0) sb_append_cstr(&sb, ", ");
                sb_append_cstr(&sb, type_to_string(type->procedure.parameters[i]));
            }
            sb_append_cstr(&sb, ") -> ");
            sb_append_cstr(&sb, type_to_string(type->procedure.return_type));

            context_arena = old;
            return sb.data;
        }

        case TYPE_STRUCT: {
            Arena *old = context_arena;
            context_arena = &temporary_arena;
            String_Builder sb = {0};

            sb_append_cstr(&sb, "{ ");
            for (size_t i = 0; i < type->structure.field_count; ++i) {
                sb_print(&sb, "%s: %s; ", type->structure.names[i], type_to_string(type->structure.types[i]));
            }
            sb_append_cstr(&sb, "}");

            context_arena = old;
            return sb.data;
        }

        case TYPE_POINTER: return tprint("*%s", type_to_string(type->pointer.element_type));

        case TYPE_ARRAY: {
            if (type->array.element_count >= 0) {
                return tprint("[%ld]%s",
                    type->array.element_count,
                    type_to_string(type->array.element_type));
            }
            return tprint("[]%s", type_to_string(type->array.element_type));
        }

        default: return "**INVALID**";
    }
}

const char *token_type_to_string(int type)
{
    if (type < 256) {
        return tprint("%c", (char) type);
    }

    switch (type) {
    case TOKEN_IDENT: return "identifier";
    case TOKEN_NUMBER: return "number";
    case TOKEN_STRING: return "string";
    case TOKEN_PLUSEQUALS: return "+=";
    case TOKEN_MINUSEQUALS: return "-=";
    case TOKEN_TIMESEQUALS: return "*=";
    case TOKEN_DIVEQUALS: return "/=";
    case TOKEN_MODEQUALS: return "%=";
    case TOKEN_ISEQUAL: return "==";
    case TOKEN_ISNOTEQUAL: return "!=";
    case TOKEN_LOGICAL_AND: return "&&";
    case TOKEN_LOGICAL_OR: return "||";
    case TOKEN_LESSEQUALS: return "<=";
    case TOKEN_GREATEREQUALS: return ">=";

    case TOKEN_RIGHT_ARROW: return "->";
    case TOKEN_DOUBLE_DOT: return "..";

    case TOKEN_POINTER_DEREFERENCE_OR_SHIFT_LEFT: return "<<";
    case TOKEN_SHIFT_RIGHT: return ">>";
    case TOKEN_BITWISE_AND_EQUALS: return "&=";
    case TOKEN_BITWISE_OR_EQUALS: return "|=";
    case TOKEN_BITWISE_XOR_EQUALS: return "^=";
    
    case TOKEN_KEYWORD_IF: return "if";
    case TOKEN_KEYWORD_THEN: return "then";
    case TOKEN_KEYWORD_ELSE: return "else";
    // case TOKEN_KEYWORD_CASE: return "case";
    case TOKEN_KEYWORD_RETURN: return "return";
    case TOKEN_KEYWORD_STRUCT: return "struct";
    case TOKEN_KEYWORD_WHILE: return "while";
    case TOKEN_KEYWORD_BREAK: return "break";
    case TOKEN_KEYWORD_CONTINUE: return "continue";
    case TOKEN_KEYWORD_USING: return "using";

    case TOKEN_KEYWORD_DEFER: return "defer";
    case TOKEN_KEYWORD_SIZE_OF: return "size_of";
    case TOKEN_KEYWORD_TYPE_OF: return "type_of";
    case TOKEN_KEYWORD_INITIALIZER_OF: return "initializer_of";
    case TOKEN_KEYWORD_TYPE_INFO: return "type_info";
    case TOKEN_KEYWORD_NULL: return "null";

    case TOKEN_KEYWORD_ENUM: return "enum";
    case TOKEN_KEYWORD_TRUE: return "true";
    case TOKEN_KEYWORD_FALSE: return "false";
    case TOKEN_KEYWORD_UNION: return "union";

    case TOKEN_NOTE: return "note";
    case TOKEN_END_OF_INPUT: return "end of input";

    case TOKEN_ERROR: return "error";

    default: return "**INVALID**";
    }
}

const char *ast_to_string(const Ast *ast)
{
    if (ast == NULL) return "**NULL**";
    
    #define xx (const Ast *)

    switch (ast->type) {
        case AST_UNINITIALIZED: return "**UNINITIALIZED**";
        
        case AST_LITERAL: {
            const Ast_Literal *lit = Down(ast);
            switch (lit->kind) {
            case LITERAL_INT:    return tprint("%llu", lit->int_value);
            case LITERAL_FLOAT:  return tprint("%f", lit->float_value);
            case LITERAL_STRING: return tprint("\"%*.s\"", lit->string_value.count, lit->string_value.data);
            }
            UNREACHABLE;
        }

        case AST_IDENT: return ((const Ast_Ident *)ast)->name;

        case AST_BINARY_OPERATOR: {
            const Ast_Binary_Operator *bin = Down(ast);
            return tprint("(%s %s %s)",
                ast_to_string(bin->left),
                token_type_to_string(bin->operator_type),
                ast_to_string(bin->right));
        }
        
        case AST_PROCEDURE_CALL: {
            const Ast_Procedure_Call *call = Down(ast);

            Arena *saved = context_arena;
            context_arena = &temporary_arena;

            String_Builder sb = {0};
            sb_print(&sb, "%s(", ast_to_string(call->procedure_expression));

            For (call->arguments) {
                if (it > 0) sb_append_cstr(&sb, ", ");
                sb_append_cstr(&sb, ast_to_string(call->arguments[it]));
            }
            
            sb_append_cstr(&sb, ")");

            context_arena = saved;
            return sb.data;
        }

        case AST_TYPE_DEFINITION: {
            const Ast_Type_Definition *defn = Down(ast);

            if (defn->struct_desc) {
                return tprint("struct %s", ast_to_string(xx &defn->struct_desc->scope));
            }

            if (defn->enum_defn) {
                return tprint("enum %s", ast_to_string(xx &defn->enum_defn->scope));
            }

            if (defn->literal_name) return defn->literal_name;

            if (defn->type_name) return ast_to_string(xx defn->type_name);

            if (defn->array_element_type) {
                return tprint("[] %s", ast_to_string(xx defn->array_element_type));
            }

            if (defn->pointer_to) {
                assert(defn->pointer_level <= 10);
                return tprint("%.*s%s", defn->pointer_level, "**********", ast_to_string(xx defn->pointer_to));
            }

            assert(defn->lambda_return_type);
            
            Arena *saved = context_arena;
            context_arena = &temporary_arena;

            String_Builder sb = {0};
            sb_append_cstr(&sb, "(");

            For (defn->lambda_argument_types) {
                if (it > 0) sb_append_cstr(&sb, ", ");
                sb_append_cstr(&sb, ast_to_string(xx defn->lambda_argument_types[it]));
            }
            
            sb_print(&sb, ") -> %s", ast_to_string(xx defn->lambda_return_type));

            context_arena = saved;
            return sb.data;
        }

        case AST_TYPE_INSTANTIATION: {
            const Ast_Type_Instantiation *inst= Down(ast);
            // TODO: print values
            return tprint("%s{}", ast_to_string(xx inst->type_definition));
        }

        case AST_BLOCK: {
            const Ast_Block *block = Down(ast);

            Arena *saved = context_arena;
            context_arena = &temporary_arena;

            String_Builder sb = {0};
            sb_append_cstr(&sb, "{ ");

            For (block->statements) {
                sb_print(&sb, "%s; ", ast_to_string(xx block->statements[it]));
            }
            
            sb_append_cstr(&sb, "}");

            context_arena = saved;
            return sb.data;
        }

        case AST_LAMBDA_BODY: {
            const Ast_Lambda_Body *body = Down(ast);
            return ast_to_string(&body->block.base);
        }

        case AST_LAMBDA: {
            const Ast_Lambda *lambda = Down(ast);
            return tprint("%s %s", ast_to_string(xx lambda->type_definition), ast_to_string(xx &lambda->body));
        }

        case AST_DECLARATION: {
            const Ast_Declaration *decl = Down(ast);
            if (decl->flags & DECLARATION_IS_COMPTIME) {
                return tprint("%s :: %s", ast_to_string(xx &decl->ident), ast_to_string(xx decl->expression));
            }
            return tprint("%s := %s", ast_to_string(xx &decl->ident), ast_to_string(xx decl->expression));
        }

        default: return "**INVALID**";
    }

    #undef xx
}
