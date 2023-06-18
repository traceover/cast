#include <stdarg.h>
#include <math.h>

#include "typecheck.h"
#include "workspace.h"

#define Replace(ast) do { \
    while (ast->replacement) ast = ast->replacement; \
} while(0)

#define Replace_Type(type) do { \
    while (type->_expression.replacement) type = xx type->_expression.replacement; \
} while(0)

// Be sure to call Replace() on this first to replace constant identifiers.
static bool expression_is_lvalue(Ast_Expression *expr)
{
    if (expr->kind == AST_IDENT) {
        Ast_Ident *ident = xx expr;
        return !(ident->resolved_declaration->flags & DECLARATION_IS_CONSTANT);
    }
    if (expr->kind == AST_SELECTOR) {
        Ast_Selector *selector = xx expr;
        return expression_is_lvalue(selector->namespace_expression);
    }
    return false;
}

void typecheck_declaration(Workspace *w, Ast_Declaration *decl)
{
#if 0
    String_Builder sb = {0};
    sb_append_cstr(&sb, ">>> ");
    print_decl_to_builder(&sb, decl);
    sb_append_cstr(&sb, "\n");
    printf(SV_Fmt, SV_Arg(sb));
#endif
    
    // Note: None of this gets set for non-constants, which is totally fine.
    while (decl->typechecking_position < arrlenu(decl->flattened)) {
        Ast_Node *node = &decl->flattened[decl->typechecking_position];
        if (node->expression) {
            typecheck_expression(w, node->expression);
            Replace(node->expression);
            if (node->expression->inferred_type) {
                decl->typechecking_position += 1;
            } else {
                // Hit a roadblock.
                Source_Location loc = node->expression->location;
                String_View name = decl->ident ? decl->ident->name : sv_from_cstr("<unnamed>");
                printf(Loc_Fmt": WAITING: Declaration '"SV_Fmt"' is waiting on: %s\n",
                    SV_Arg(w->files[loc.fid].path),
                    Loc_Arg(loc),
                    SV_Arg(name),
                    expr_to_string(node->expression));
                return;
            }
        }
        if (node->statement) {
            typecheck_statement(w, node->statement);
            if (node->statement->typechecked) {
                decl->typechecking_position += 1;
            } else {
                // Currently this can never happen because we can never wait on statements.
                // Their inner expressions are typechecked before they are.
                printf("$$$ %s\n", stmt_to_string(node->statement));
                return;
            }
        }
    }

    // We are done typechecking our members.

    if (decl->flags & DECLARATION_IS_PROCEDURE_BODY) goto done;

    if (decl->flags & DECLARATION_IS_CONSTANT) {
        if (!decl->root_expression) {
            report_error(w, decl->location, "Constant declarations must have a value (this is an internal error).");
        }
    }

    if (!decl->my_type) {
        if (!decl->root_expression) {
            report_error(w, decl->location, "We have a non-constant declaration with no type and no value (this is an internal error).");
        }

        Replace(decl->root_expression);
        assert(decl->root_expression->inferred_type);

        if (decl->root_expression->kind == AST_LITERAL) {
            Ast_Literal *literal = xx decl->root_expression;
            switch (literal->kind) {
            case LITERAL_STRING: decl->my_type = w->type_def_string; break;
            case LITERAL_BOOL:   decl->my_type = w->type_def_bool;   break;
            case LITERAL_NULL:
                report_error(w, decl->location, "Cannot assign null to declaration with no type.");
            }
        } else {
            decl->my_type = decl->root_expression->inferred_type;
        }

        decl->flags |= DECLARATION_TYPE_WAS_INFERRED_FROM_EXPRESSION;
        goto done;
    }

    // So we know we have a type now.
    Replace_Type(decl->my_type);

    if (decl->my_type == w->type_def_void) {
        report_error(w, decl->location, "Cannot have a declaration with void type.");
    }

    if (!decl->root_expression) {
        // We're definitely not a constant (this error is checked above).
        // So we just set the default value for the type.
        decl->root_expression = generate_default_value_for_type(w, decl->my_type);
        decl->root_expression->location = decl->location;
        decl->root_expression->inferred_type = decl->my_type;
        decl->flags |= DECLARATION_VALUE_WAS_INFERRED_FROM_TYPE;
        goto done;       
    }

    // TODO: Because we constant replace the expression, the debug location information gets messed up.
    Replace(decl->root_expression);

    // TODO: String to char.
    // Check if my_type is a numeric type and root_expression is a literal.

    if (decl->root_expression->kind == AST_NUMBER) {
        typecheck_number(w, xx decl->root_expression, decl->my_type);
    } else {
        // Check that we're not doing something like `x: bool = 12345`.
        if (!types_are_equal(decl->my_type, decl->root_expression->inferred_type)) {
            report_error(w, decl->root_expression->location, "Type mismatch: Wanted %s but got %s.",
                type_to_string(decl->my_type), type_to_string(decl->root_expression->inferred_type));
        }
    }

done:
    decl->flags |= DECLARATION_HAS_BEEN_TYPECHECKED;
}

Ast_Expression *generate_default_value_for_type(Workspace *w, Ast_Type_Definition *type)
{
    if (type->pointer_to) {
        return xx make_literal(LITERAL_NULL);
    }

    if (type->lambda_return_type) {
        return xx make_literal(LITERAL_NULL);
    }
    
    if (type->struct_desc) {
        // The default value of a structure is one that has been initialized properly.
        Ast_Type_Instantiation *inst = context_alloc(sizeof(*inst));
        inst->type_definition = type;
        return xx inst;
    }

    if (type->flags & TYPE_IS_NUMERIC) {
        return xx make_number(0);
    }

    if (type->flags & TYPE_IS_LITERAL) {
        return xx make_literal(type->literal_kind);
    }

    UNIMPLEMENTED;

    (void)w;
    return NULL;
}

Ast_Expression *autocast_to_bool(Workspace *w, Ast_Expression *expr)
{
    Ast_Type_Definition *defn = expr->inferred_type;

    if (defn->literal_name) {
        if (defn->number_literal_low) {
            Ast_Binary_Operator *binary = context_alloc(sizeof(*binary));
            binary->_expression.location = expr->location;
            binary->_expression.inferred_type = w->type_def_bool;
            binary->left = expr;
            binary->operator_type = TOKEN_ISNOTEQUAL;
            binary->right = xx make_number(0);
            return xx binary;
        }
        if (defn == w->type_def_string) {
            // TODO: Create a selector and get the "count" field and then check if it's not zero.
            assert(0);
        }
        if (defn == w->type_def_void) {
            report_error(w, expr->location, "Cannot use value with void type.");
        }
        assert(0);
    }

    if (defn->pointer_to) {
        Ast_Literal *literal = make_literal(LITERAL_NULL);
        literal->_expression.location = expr->location;
        literal->_expression.inferred_type = defn;
        
        Ast_Binary_Operator *binary = context_alloc(sizeof(*binary));
        binary->_expression.kind = AST_BINARY_OPERATOR;
        binary->_expression.location = expr->location;
        binary->_expression.inferred_type = w->type_def_bool;
        binary->left = expr;
        binary->operator_type = TOKEN_ISNOTEQUAL;
        binary->right = xx literal;
        return xx binary;
    }

    return NULL;
}

void typecheck_number(Workspace *w, Ast_Number *number, Ast_Type_Definition *supplied_type)
{
    if (!supplied_type) {
        if (number->flags & NUMBER_FLAGS_FLOAT64) number->_expression.inferred_type = w->type_def_float64;
        if (number->flags & NUMBER_FLAGS_FLOAT)   number->_expression.inferred_type = w->type_def_float;
        else                                      number->_expression.inferred_type = w->type_def_int;
        return;
    }

    if (!(supplied_type->flags & TYPE_IS_NUMERIC)) {
        report_error(w, number->_expression.location, "Type mismatch: Wanted %s but got a number literal.", type_to_string(supplied_type));
    }

    if (number->flags & NUMBER_FLAGS_FLOAT) {
        if (!(supplied_type->number_flags & NUMBER_FLAGS_FLOAT)) {
            report_error(w, number->_expression.location, "Cannot use float literal as type %s.", type_to_string(supplied_type));
        }
        if (number->flags & NUMBER_FLAGS_FLOAT64 && !(supplied_type->number_flags & NUMBER_FLAGS_FLOAT64)) {
            report_error(w, number->_expression.location, "Precision loss when casting to %s.", type_to_string(supplied_type));
        }
        goto done;
    }

    // Number literal with no fraction can be int or float.

    if (supplied_type->number_flags & NUMBER_FLAGS_FLOAT) {
        goto done;
    }

    // We know the type is an integer, so let's compare the range.

    if (supplied_type->number_flags & NUMBER_FLAGS_SIGNED) {
        signed long low = supplied_type->number_literal_low->as.integer;
        signed long high = supplied_type->number_literal_high->as.integer;
        signed long value = number->as.integer;
        if (value > high) {
            report_error(w, number->_expression.location, "Numeric constant too big for type (max for %s is %lu).", supplied_type->literal_name, high);
        }
        if (value < low) {
            report_error(w, number->_expression.location, "Numeric constant too small for type (min for %s is %lu).", supplied_type->literal_name, low);
        }
        goto done;
    }
    
    unsigned long low = supplied_type->number_literal_low->as.integer;
    unsigned long high = supplied_type->number_literal_high->as.integer;

    // TODO: we need to compare differently based on signedness.
    
    if (number->as.integer > high) {
        report_error(w, number->_expression.location, "Numeric constant too big for type (max for %s is %lu).", supplied_type->literal_name, high);
    }
    if (number->as.integer < low) {
        report_error(w, number->_expression.location, "Numeric constant too small for type (min for %s is %lu).", supplied_type->literal_name, low);
    }
    
done:
    number->_expression.inferred_type = supplied_type;
}

void typecheck_literal(Workspace *w, Ast_Literal *literal)
{
    switch (literal->kind) {
    case LITERAL_BOOL:   literal->_expression.inferred_type = w->type_def_bool;   break;
    case LITERAL_STRING: literal->_expression.inferred_type = w->type_def_string; break;
    case LITERAL_NULL:   literal->_expression.inferred_type = w->type_def_void;   break;
    }

    // if (literal->default_type == w->type_def_string) {
    //     if (supplied_type->number_flags & NUMBER_FLAGS_NUMBER && !(supplied_type->number_flags & NUMBER_FLAGS_FLOAT)) {
    //         if (literal->string_value.count != 1) {
    //             report_error(w, literal->_expression.location, "String literals can only cast to int if they are exactly 1 character.");
    //         }
            
    //         Ast_Literal *char_literal = context_alloc(sizeof(*char_literal));
    //         char_literal->_expression.location = literal->_expression.location;
    //         char_literal->_expression.inferred_type = supplied_type;
    //         char_literal->default_type = supplied_type;
    //         char_literal->number_flags = NUMBER_FLAGS_NUMBER;
    //         char_literal->integer_value = *literal->string_value.data;

    //         literal->_expression.replacement = xx char_literal;
    //         return;
    //     }
    //     goto error;
    // }
}

void typecheck_identifier(Workspace *w, Ast_Ident *ident)
{
    // TODO: We should have a separate phase where we check for circular dependencies and unresolved identifiers.
    if (!ident->resolved_declaration) {
        ident->resolved_declaration = find_declaration_from_identifier(ident);
        if (!ident->resolved_declaration) {
            report_error(w, ident->_expression.location, "Undeclared identifier '"SV_Fmt"'.", SV_Arg(ident->name));
        }
    }

    Ast_Declaration *decl = ident->resolved_declaration;

    // This is where we might hit a roadblock.

    if (!(decl->flags & DECLARATION_HAS_BEEN_TYPECHECKED)) {
        if (!(decl->flags & DECLARATION_IS_CONSTANT)) {
            report_error(w, ident->_expression.location, "Cannot use variable '"SV_Fmt"' before it is defined.", SV_Arg(ident->name));
        }
        // Otherwise we must wait for the constant to come in.
        return;
    }

    // If the declaration has been typechecked, we can typecheck ourselves. 

    assert(decl->my_type);

    if (decl->flags & DECLARATION_IS_CONSTANT) {
        // TODO: does this work for lambdas?
        ident->_expression.replacement = decl->root_expression;
    }

    ident->_expression.inferred_type = decl->my_type;

    UNUSED(w);
}

void typecheck_unary_operator(Workspace *w, Ast_Unary_Operator *unary)
{
#define site (unary->_expression.location)

    Replace(unary->subexpression);
    
    if (unary->operator_type == '*') {
        if (!expression_is_lvalue(unary->subexpression)) {
            report_error(w, site, "Can only take a pointer to an lvalue."); // TODO: This error mesage.
        }
        Ast_Type_Definition *defn = context_alloc(sizeof(*defn));
        defn->pointer_to = unary->subexpression->inferred_type;
        defn->_expression.inferred_type = w->type_def_type; // TODO: Is this really necessary?
        defn->_expression.location = unary->subexpression->inferred_type->_expression.location;
        unary->_expression.inferred_type = defn;
        return;
    }
    
    UNIMPLEMENTED;

#undef site
}

inline Ast_Literal *make_literal(Literal_Kind kind)
{
    Ast_Literal *lit = context_alloc(sizeof(*lit));
    lit->_expression.kind = AST_LITERAL;
    lit->kind = kind;
    return lit;
}

inline Ast_Literal *make_boolean(Workspace *w, Source_Location loc, bool value)
{
    Ast_Literal *lit = make_literal(LITERAL_BOOL);
    lit->bool_value = value;
    lit->_expression.location = loc;
    lit->_expression.inferred_type = w->type_def_bool;
    return lit;
}

inline Ast_Number *make_float_or_float64(Workspace *w, Source_Location loc, double value, bool use_float64)
{
    Ast_Number *res = make_number_float(value);
    res->_expression.location = loc;
    if (use_float64) {
        res->flags |= NUMBER_FLAGS_FLOAT64;
        res->_expression.inferred_type = w->type_def_float64;
    } else {
        res->_expression.inferred_type = w->type_def_float;
    }
    return res;
}

inline Ast_Number *make_integer(Workspace *w, Source_Location loc, unsigned long value, bool is_signed)
{
    Ast_Number *res = make_number(value);
    res->_expression.location = loc;
    res->_expression.inferred_type = w->type_def_int;
    if (is_signed) res->flags |= NUMBER_FLAGS_SIGNED;
    return res;
}

Ast_Expression *fold_binary_arithmetic_or_comparison(Workspace *w, char operator_type, Ast_Number *left, Ast_Number *right)
{
    Source_Location loc = left->_expression.location;

    // If either is float, we are float.
    if ((left->flags & NUMBER_FLAGS_FLOAT) || (right->flags & NUMBER_FLAGS_FLOAT)) {
        bool use_float64 = (left->flags & NUMBER_FLAGS_FLOAT64) || (right->flags & NUMBER_FLAGS_FLOAT64);
        switch ((int)operator_type) {
        case '+':                 return xx make_float_or_float64(w, loc, left->as.real + right->as.real, use_float64);
        case '-':                 return xx make_float_or_float64(w, loc, left->as.real - right->as.real, use_float64);
        case '*':                 return xx make_float_or_float64(w, loc, left->as.real * right->as.real, use_float64);
        case '/':                 return xx make_float_or_float64(w, loc, left->as.real / right->as.real, use_float64);
        case '%':                 return xx make_float_or_float64(w, loc, fmod(left->as.real, right->as.real), use_float64);
        case '>':                 return xx make_boolean(w, loc, left->as.real > right->as.real);
        case '<':                 return xx make_boolean(w, loc, left->as.real < right->as.real);
        case TOKEN_GREATEREQUALS: return xx make_boolean(w, loc, left->as.real >= right->as.real);
        case TOKEN_LESSEQUALS:    return xx make_boolean(w, loc, left->as.real <= right->as.real);
        default:  assert(0);
        }
    }

    // If either is signed, we are signed.
    if ((left->flags & NUMBER_FLAGS_SIGNED) || (right->flags & NUMBER_FLAGS_SIGNED)) {
        signed long l = left->as.integer;
        signed long r = right->as.integer;
        switch ((int)operator_type) {
        case '+':                 return xx make_integer(w, loc, l + r, true);
        case '-':                 return xx make_integer(w, loc, l - r, true);
        case '*':                 return xx make_integer(w, loc, l * r, true);
        case '/':                 return xx make_integer(w, loc, l / r, true);
        case '%':                 return xx make_integer(w, loc, l % r, true);
        case '>':                 return xx make_boolean(w, loc, l >  r);
        case '<':                 return xx make_boolean(w, loc, l <  r);
        case TOKEN_GREATEREQUALS: return xx make_boolean(w, loc, l >= r);
        case TOKEN_LESSEQUALS:    return xx make_boolean(w, loc, l <= r);
        default:  assert(0);
        }
    }
    
    unsigned long l = left->as.integer;
    unsigned long r = right->as.integer;
    switch ((int)operator_type) {
    case '+':                 return xx make_integer(w, loc, l +  r, false);
    case '-':                 return xx make_integer(w, loc, l -  r, false);
    case '*':                 return xx make_integer(w, loc, l *  r, false);
    case '/':                 return xx make_integer(w, loc, l /  r, false);
    case '%':                 return xx make_integer(w, loc, l %  r, false);
    case '>':                 return xx make_boolean(w, loc, l >  r);
    case '<':                 return xx make_boolean(w, loc, l <  r);
    case TOKEN_GREATEREQUALS: return xx make_boolean(w, loc, l >= r);
    case TOKEN_LESSEQUALS:    return xx make_boolean(w, loc, l <= r);
    default:  assert(0);
    }
}

Ast_Type_Definition *typecheck_binary_arithmetic(Workspace *w, char operator_type, Source_Location site, Ast_Expression *left, Ast_Expression *right)
{
    // Check for pointer arithmetic.
    if (left->inferred_type->pointer_to) {
        if (right->inferred_type->pointer_to) {
            // Pointer and pointer.
            if (!pointer_types_are_equal(left->inferred_type, right->inferred_type)) {
                report_error(w, site, "Type mismatch: Cannot perform pointer arithmetic on points of different types (got %s and %s).",
                    type_to_string(left->inferred_type), type_to_string(right->inferred_type));
            }
        } else {
            // Pointer and integer.
            if (!(right->inferred_type->flags & TYPE_IS_NUMERIC)) {
                report_error(w, right->location, "Type mismatch: Pointer arithmetic operand must be a number (got %s).",
                    type_to_string(right->inferred_type));
            }
            if (right->inferred_type->number_flags & NUMBER_FLAGS_FLOAT) {
                report_error(w, right->location, "Type mismatch: Pointer arithmetic operand must be an integer (got %s).",
                    type_to_string(right->inferred_type));
            }
        }
        return left->inferred_type;
    }

    // The types must be equal, and they also must be numbers.

    if (!types_are_equal(left->inferred_type, right->inferred_type)) {
        report_error(w, site, "Type mismatch: Types on either side of '%s' must be the same (got %s and %s).",
            token_type_to_string(operator_type), type_to_string(left->inferred_type), type_to_string(right->inferred_type));
    }

    Ast_Type_Definition *defn = left->inferred_type; // Now they are the same.

    if (!(defn->flags & TYPE_IS_NUMERIC)) {
        report_error(w, site, "Type mismatch: Operator '%s' does not work on non-number types (got %s).",
            token_type_to_string(operator_type), type_to_string(defn));
    }

    // I don't think we need to compare signedness here because we checked that types_are_equal.

    return defn;
}

void typecheck_binary_comparison(Workspace *w, char operator_type, Source_Location site, Ast_Expression *left, Ast_Expression *right)
{
    if (!types_are_equal(left->inferred_type, right->inferred_type)) {
        report_error(w, site, "Type mismatch: Types on either side of '%s' must be the same (got %s and %s).",
            token_type_to_string(operator_type), type_to_string(left->inferred_type), type_to_string(right->inferred_type));
    }

    Ast_Type_Definition *defn = left->inferred_type; // Now they are the same.

    if (!(defn->flags & TYPE_IS_NUMERIC)) {
        report_error(w, site, "Type mismatch: Operator '%s' does not work on non-number types (got %s).",
            token_type_to_string(operator_type), type_to_string(defn));
    }

    // I don't think we need to compare signedness here because we checked that types_are_equal.
}

// @Cleanup: This whole function's error messages.
void typecheck_binary_operator(Workspace *w, Ast_Binary_Operator *binary)
{
    // Check for constant replacement.
    Ast_Expression *left = binary->left;
    Ast_Expression *right = binary->right;
    Replace(left);
    Replace(right);

    switch (binary->operator_type) {
    case '+':
    case '-':
    case '*':
    case '/':
    case '%':
        // If left and right are literals, replace us with a literal.
        // Technically, LLVM does this for us, but let's not rely on that.
        if (left->kind == AST_NUMBER && right->kind == AST_NUMBER) {
            binary->_expression.replacement = fold_binary_arithmetic_or_comparison(w, (char)binary->operator_type, xx left, xx right);
            break;
        }
        binary->_expression.inferred_type = typecheck_binary_arithmetic(w, (char)binary->operator_type, binary->_expression.location, left, right);
        break;
    case TOKEN_ISEQUAL:
    case TOKEN_ISNOTEQUAL:
        if (!types_are_equal(left->inferred_type, right->inferred_type)) {
            report_error(w, binary->_expression.location, "Type mismatch: Cannot compare values of different types (got %s and %s).",
                type_to_string(left->inferred_type), type_to_string(right->inferred_type));
        }
        binary->_expression.inferred_type = w->type_def_bool;
        // TODO: fold_binary_comparison
        break;
    case '>':
    case '<':
    case TOKEN_GREATEREQUALS:
    case TOKEN_LESSEQUALS:
        // If left and right are literals, replace us with a literal.
        // Technically, LLVM does this for us, but let's not rely on that.
        if (left->kind == AST_NUMBER && right->kind == AST_NUMBER) {
            binary->_expression.replacement = fold_binary_arithmetic_or_comparison(w, (char)binary->operator_type, xx left, xx right);
            break;
        }
        typecheck_binary_comparison(w, (char)binary->operator_type, binary->_expression.location, left, right);
        binary->_expression.inferred_type = w->type_def_bool;
        break;
    case TOKEN_LOGICAL_AND:
    case TOKEN_LOGICAL_OR:
        if (left->inferred_type != w->type_def_bool) {
            report_error(w, left->location, "Type mismatch: Operator '%s' only works on boolean types (got %s).",
                token_type_to_string(binary->operator_type), type_to_string(left->inferred_type));
        }
        if (right->inferred_type != w->type_def_bool) {
            report_error(w, right->location, "Type mismatch: Operator '%s' only works on boolean types (got %s).",
                token_type_to_string(binary->operator_type), type_to_string(right->inferred_type));
        }
        binary->_expression.inferred_type = w->type_def_bool;
        break;
    case TOKEN_POINTER_DEREFERENCE_OR_SHIFT_LEFT:
    case TOKEN_SHIFT_RIGHT:
    case TOKEN_ARRAY_SUBSCRIPT:
        UNIMPLEMENTED;
    }
}

void typecheck_lambda(Workspace *w, Ast_Lambda *lambda)
{
    lambda->_expression.inferred_type = lambda->type_definition;
    UNUSED(w);
    // UNUSED(lambda);
    // UNIMPLEMENTED;
}

void typecheck_procedure_call(Workspace *w, Ast_Procedure_Call *call)
{
    if (!call->procedure_expression->inferred_type->lambda_return_type) {
        report_error(w, call->procedure_expression->location, "Type mismatch: Wanted a procedure but got %s.",
            type_to_string(call->procedure_expression->inferred_type));
    }

    Ast_Type_Definition *proc = call->procedure_expression->inferred_type;

    size_t n = arrlenu(call->arguments);
    size_t m = arrlenu(proc->lambda_argument_types);
    
    if (n > m) report_error(w, call->_expression.location, "Too many arguments for procedure call (wanted %zu but got %zu).", m, n);
    if (m > n) report_error(w, call->_expression.location, "Not enough arguments for procedure call (wanted %zu but got %zu).", m, n);

    for (size_t i = 0; i < n; ++i) {
        // TODO: Handle number constants.
        if (!types_are_equal(call->arguments[i]->inferred_type, proc->lambda_argument_types[i])) {
            report_error(w, call->arguments[i]->location, "Argument type mismatch: Wanted %s but got %s.",
                type_to_string(proc->lambda_argument_types[i]), type_to_string(call->arguments[i]->inferred_type));
        }
    }

    // @Incomplete: This doesn't work for multiple return values.
    call->_expression.inferred_type = proc->lambda_return_type;
}

inline void typecheck_definition(Workspace *w, Ast_Type_Definition *defn)
{
    if (defn->struct_desc) {
        if (!defn->struct_desc->block->_statement.typechecked) return;
        For (defn->struct_desc->block->declarations) {
            Ast_Declaration *member = defn->struct_desc->block->declarations[it];
            if (member->flags & DECLARATION_IS_STRUCT_FIELD) {
                assert(member->my_type);
                arrput(defn->struct_desc->field_types, member->my_type);
            }
        }
    }

    if (defn->type_name) {
        Ast_Declaration *decl = defn->type_name->resolved_declaration;

        if (decl->my_type != w->type_def_type) {
            report_error(w, defn->_expression.location, "Type mismatch: Wanted Type but got %s.",
                type_to_string(decl->my_type));
        }

        defn->_expression.replacement = xx decl->root_expression;
    }

    defn->_expression.inferred_type = w->type_def_type;
}

void typecheck_cast(Workspace *w, Ast_Cast *cast)
{
    UNUSED(w);
    cast->_expression.inferred_type = cast->type;
}

void typecheck_selector(Workspace *w, Ast_Selector *selector)
{
    Replace(selector->namespace_expression);

    Ast_Type_Definition *defn = selector->namespace_expression->inferred_type;
    Replace_Type(defn);

    // Our syntax for pointer dereference is currently `pointer.*` so we use a selector.
    if (selector->is_pointer_dereference) {
        if (!defn->pointer_to) {
            report_error(w, selector->_expression.location, "Attempt to dereference a non-pointer (got type %s).",
                type_to_string(defn));
        }
        selector->_expression.inferred_type = defn->pointer_to;
        return;
    }

    // Otherwise we actually need to do a member lookup.

    if (selector->resolved_declaration) {
        // Since we may be waiting for this declaration, we will try to be typechecked multiple times.
        // We cache the resolved_declaration for that reason.

        Ast_Declaration *decl = selector->resolved_declaration;

        if (!(decl->flags & DECLARATION_HAS_BEEN_TYPECHECKED)) return;

        // Otherwise, we're done.
        selector->_expression.inferred_type = decl->my_type;
        if (decl->flags & DECLARATION_IS_CONSTANT) {
            selector->_expression.replacement = decl->root_expression;
        }
        if (decl->flags & DECLARATION_IS_STRUCT_FIELD) {
            selector->struct_field_index = decl->struct_field_index;
        }
        return;
    }
    
    if (defn->flags & TYPE_IS_NAMESPACE) {
        if (defn == w->type_def_string) {
            if (sv_eq(selector->ident->name, sv_from_cstr("data"))) {
                selector->struct_field_index = 0;

                Ast_Type_Definition *u8_ptr_defn = context_alloc(sizeof(Ast_Type_Definition));
                u8_ptr_defn->_expression.kind = AST_TYPE_DEFINITION;
                u8_ptr_defn->_expression.inferred_type = w->type_def_type;
                u8_ptr_defn->pointer_to = w->type_def_u8;

                selector->_expression.inferred_type = u8_ptr_defn;
                return;
            }
            if (sv_eq(selector->ident->name, sv_from_cstr("count"))) {
                selector->struct_field_index = 1;
                selector->_expression.inferred_type = w->type_def_int;
                return;
            }
            report_error(w, selector->_expression.location, "String type has no member '"SV_Fmt"'.", SV_Arg(selector->ident->name));
        }

        if (defn->struct_desc) {
            Ast_Declaration *decl = find_declaration_in_block(defn->struct_desc->block, selector->ident->name);
            if (!decl) {
                report_error(w, selector->_expression.location, "Struct has no member '"SV_Fmt"'.", SV_Arg(selector->ident->name));
            }

            // Cache this in case we can't proceed and need to return here later.
            selector->resolved_declaration = decl;

            if (!(decl->flags & DECLARATION_HAS_BEEN_TYPECHECKED)) return;

            // @Copypasta
            selector->_expression.inferred_type = decl->my_type;
            if (decl->flags & DECLARATION_IS_CONSTANT) {
                selector->_expression.replacement = decl->root_expression;
            }
            if (decl->flags & DECLARATION_IS_STRUCT_FIELD) {
                selector->struct_field_index = decl->struct_field_index;
            }
            return;
        }

        assert(0);
    }

    report_error(w, selector->namespace_expression->location, "Attempt to dereference a non-namespaced type (got type %s).",
        type_to_string(defn));
}

void typecheck_instantiation(Workspace *w, Ast_Type_Instantiation *inst)
{
    Ast_Type_Definition *defn = inst->type_definition;
    Source_Location site = inst->_expression.location;

    while (inst->type_definition->_expression.replacement) {
        inst->type_definition = xx inst->type_definition->_expression.replacement;
    }

    if (defn->pointer_to) {
        report_error(w, site, "Cannot instantiate a pointer type using initializer argument list.");
    }

    if (defn->array_element_type) {
        report_error(w, site, "Cannot instantiate an array type using initializer argument list (this is not implemented yet).");
    }

    if (defn->struct_desc) {
        int n = arrlen(inst->arguments);
        int m = defn->struct_desc->field_count;
        if (n != m) {
            report_error(w, site, "Incorrect number of arguments to instantiate struct type (wanted %d but got %d).", m, n);
        }
        for (int i = 0; i < n; ++i) {
            Ast_Type_Definition *expected = defn->struct_desc->field_types[i];
            Ast_Type_Definition *actual = inst->arguments[i]->inferred_type;
            if (!types_are_equal(actual, expected)) {
                report_error(w, inst->arguments[i]->location, "Field type mismatch: Wanted %s but got %s.",
                    type_to_string(expected), type_to_string(actual));
            }
        }
    }

    if (defn->flags & TYPE_IS_LITERAL) {
        if (arrlen(inst->arguments) > 1) {
            report_error(w, site, "Can only instantiate literal types with 1 argument.");
        }

        if (inst->arguments[0]->kind == AST_NUMBER) {
            typecheck_number(w, xx inst->arguments[0], defn);
        }

        inst->_expression.replacement = inst->arguments[0];
    }

    inst->_expression.inferred_type = defn;
}

void typecheck_expression(Workspace *w, Ast_Expression *expr)
{
    if (expr->inferred_type) return; // TODO: replace this with an assert and see if this ever happens.
    switch (expr->kind) {
    case AST_NUMBER:             typecheck_number(w, xx expr, NULL);    break;
    case AST_LITERAL:            typecheck_literal(w, xx expr);         break;
    case AST_IDENT:              typecheck_identifier(w, xx expr);      break;
    case AST_UNARY_OPERATOR:     typecheck_unary_operator(w, xx expr);  break;
    case AST_BINARY_OPERATOR:    typecheck_binary_operator(w, xx expr); break;
    case AST_LAMBDA:             typecheck_lambda(w, xx expr);          break;
    case AST_PROCEDURE_CALL:     typecheck_procedure_call(w, xx expr);  break;
    case AST_TYPE_DEFINITION:    typecheck_definition(w, xx expr);      break;
    case AST_CAST:               typecheck_cast(w, xx expr);            break;
    case AST_SELECTOR:           typecheck_selector(w, xx expr);        break;
    case AST_TYPE_INSTANTIATION: typecheck_instantiation(w, xx expr);   break;
    }
}

void typecheck_while(Workspace *w, Ast_While *while_stmt)
{
    // TODO: implicit bool conversions.

    Replace(while_stmt->condition_expression);

    if (while_stmt->condition_expression->inferred_type != w->type_def_bool) {
        report_error(w, while_stmt->condition_expression->location, "Condition of 'while' statement must result in a boolean value (got %s).",
            type_to_string(while_stmt->condition_expression->inferred_type));
    }
}

void typecheck_if(Workspace *w, Ast_If *if_stmt)
{
    // TODO: implicit bool conversions.

    Replace(if_stmt->condition_expression);

    if (if_stmt->condition_expression->inferred_type != w->type_def_bool) {
        report_error(w, if_stmt->condition_expression->location, "Condition of 'if' statement must result in a boolean value (got %s).",
            type_to_string(if_stmt->condition_expression->inferred_type));
    }
}

void typecheck_return(Workspace *w, Ast_Return *ret)
{
    UNUSED(w);

    Ast_Type_Definition *expected_type = ret->lambda_i_belong_to->type_definition->lambda_return_type;
    Ast_Type_Definition *actual_type = ret->subexpression->inferred_type;

    if (!types_are_equal(expected_type, actual_type)) {
        report_error(w, ret->subexpression->location, "Return type mismatch: Wanted %s but got %s.",
            type_to_string(expected_type), type_to_string(actual_type));
    }
}

void typecheck_using(Workspace *w, Ast_Using *using)
{
    UNUSED(w);
    UNUSED(using);
    UNIMPLEMENTED;
}

inline void typecheck_variable(Workspace *w, Ast_Variable *var)
{
    typecheck_declaration(w, var->declaration);
}

void typecheck_assignment(Workspace *w, Ast_Assignment *assign)
{
    assert(assign->pointer->kind == AST_IDENT); // TODO: a.b.c

    Ast_Ident *ident = xx assign->pointer;
    if (ident->resolved_declaration->flags & DECLARATION_IS_CONSTANT) {
        report_error(w, ident->_expression.location, "Cannot assign to constant.");
    }

    Ast_Type_Definition *expected = assign->pointer->inferred_type;
    Ast_Type_Definition *actual = assign->value->inferred_type;
    if (!types_are_equal(actual, expected)) {
        report_error(w, assign->value->location, "Type mismatch: Wanted %s but got %s.",
            type_to_string(expected), type_to_string(actual));
    }
}

void typecheck_statement(Workspace *w, Ast_Statement *stmt)
{
    if (stmt->typechecked) return;
    switch (stmt->kind) {
    case AST_BLOCK:                break; // Do nothing, our members should have compiled first.
    case AST_WHILE:                typecheck_while(w, xx stmt); break;
    case AST_IF:                   typecheck_if(w, xx stmt); break;
    case AST_LOOP_CONTROL:         break;
    case AST_RETURN:               typecheck_return(w, xx stmt); break;
    case AST_USING:                typecheck_using(w, xx stmt); break;
    case AST_IMPORT:               break;
    case AST_EXPRESSION_STATEMENT: break;
    case AST_VARIABLE:             typecheck_variable(w, xx stmt); break;
    case AST_ASSIGNMENT:           typecheck_assignment(w, xx stmt); break;
    }
    stmt->typechecked = true;
}

void flatten_expr_for_typechecking(Ast_Declaration *root, Ast_Expression *expr)
{
    if (expr == NULL) return; // This can happen if flatten_stmt_for_typechecking adds us when we don't exist.

    switch (expr->kind) {
    case AST_NUMBER:
    case AST_LITERAL:
    case AST_IDENT:
        break;
    case AST_UNARY_OPERATOR: {
        Ast_Unary_Operator *unary = xx expr;
        flatten_expr_for_typechecking(root, unary->subexpression);
        break;
    }
    case AST_BINARY_OPERATOR: {
        Ast_Binary_Operator *binary = xx expr;
        flatten_expr_for_typechecking(root, binary->left);
        flatten_expr_for_typechecking(root, binary->right);
        break;
    }
    case AST_LAMBDA: {
        Ast_Lambda *lambda = xx expr;
        flatten_expr_for_typechecking(root, xx lambda->type_definition);
        // Notice how we don't do anything about lambda->my_body_declaration.
        // It will get async processing on its own.
        break;
    }
    case AST_PROCEDURE_CALL: {
        Ast_Procedure_Call *call = xx expr;
        flatten_expr_for_typechecking(root, call->procedure_expression);
        For (call->arguments) flatten_expr_for_typechecking(root, call->arguments[it]);
        break;
    }
    case AST_TYPE_DEFINITION: {
        Ast_Type_Definition *defn = xx expr;
        if (defn->struct_desc) flatten_stmt_for_typechecking(root, xx defn->struct_desc->block);
        if (defn->type_name)   flatten_expr_for_typechecking(root, xx defn->type_name);
        break;
    }
    case AST_CAST: {
        Ast_Cast *cast = xx expr;
        flatten_expr_for_typechecking(root, cast->subexpression);
        break;
    }
    case AST_SELECTOR: {
        Ast_Selector *selector = xx expr;
        flatten_expr_for_typechecking(root, selector->namespace_expression);
        // flatten_expr_for_typechecking(root, xx selector->ident);
        break;
    }
    case AST_TYPE_INSTANTIATION: {
        Ast_Type_Instantiation *inst = xx expr;
        flatten_expr_for_typechecking(root, xx inst->type_definition);
        For (inst->arguments) {
            flatten_expr_for_typechecking(root, inst->arguments[it]);
        }
        break;
    }
    }

    Ast_Node node;
    node.expression = expr;
    node.statement = NULL;
    arrput(root->flattened, node);
}

void flatten_stmt_for_typechecking(Ast_Declaration *root, Ast_Statement *stmt)
{
    switch (stmt->kind) {
    case AST_BLOCK: {
        Ast_Block *block = xx stmt;
        For (block->statements) flatten_stmt_for_typechecking(root, block->statements[it]);
        For (block->declarations) {
            Ast_Declaration *decl = block->declarations[it];
            flatten_expr_for_typechecking(root, decl->root_expression);
            if (decl->my_block) {
                flatten_stmt_for_typechecking(root, xx decl->my_block);
            }
        }
        break;
    }
    case AST_WHILE: {
        Ast_While *while_stmt = xx stmt;
        flatten_expr_for_typechecking(root, while_stmt->condition_expression);
        flatten_stmt_for_typechecking(root, while_stmt->then_statement);
        break;
    }
    case AST_IF: {
        Ast_If *if_stmt = xx stmt;
        flatten_expr_for_typechecking(root, if_stmt->condition_expression);
        flatten_stmt_for_typechecking(root, if_stmt->then_statement);
        if (if_stmt->else_statement) flatten_stmt_for_typechecking(root, if_stmt->else_statement);
        break;
    }
    // case AST_FOR:
    case AST_LOOP_CONTROL:
        break;
    case AST_RETURN: {
        Ast_Return *ret = xx stmt;
        flatten_expr_for_typechecking(root, ret->subexpression);
        break;
    }
    case AST_USING: {
        Ast_Using *using = xx stmt;
        flatten_expr_for_typechecking(root, using->subexpression);
        break;
    }
    case AST_IMPORT:
        break;
    case AST_EXPRESSION_STATEMENT: {
        Ast_Expression_Statement *expr = xx stmt;
        flatten_expr_for_typechecking(root, expr->subexpression);
        break;
    }
    case AST_VARIABLE: {
        Ast_Variable *var = xx stmt;
        // TODO: Is this right?
        flatten_expr_for_typechecking(root, var->declaration->root_expression);
        break;
    }
    case AST_ASSIGNMENT: {
        Ast_Assignment *assign = xx stmt;
        // TODO: Check the order on this.
        flatten_expr_for_typechecking(root, assign->value);
        flatten_expr_for_typechecking(root, assign->pointer);
        break;
    }
    }

    Ast_Node node;
    node.expression = NULL;
    node.statement = stmt;
    arrput(root->flattened, node);
}

void flatten_decl_for_typechecking(Ast_Declaration *decl)
{
    if (decl->flags & DECLARATION_IS_PROCEDURE_BODY) {
        if (decl->flags & DECLARATION_IS_FOREIGN) return; // Nothing to typecheck.
        flatten_stmt_for_typechecking(decl, xx decl->my_block->parent); // Arguments.
        flatten_stmt_for_typechecking(decl, xx decl->my_block);
        return;
    }

    if (decl->my_type) {
        flatten_expr_for_typechecking(decl, xx decl->my_type);
    }

    if (decl->root_expression) {
        flatten_expr_for_typechecking(decl, decl->root_expression);
    }
}

bool pointer_types_are_equal(Ast_Type_Definition *x, Ast_Type_Definition *y)
{
    return types_are_equal(x->pointer_to, y->pointer_to);
}

bool types_are_equal(Ast_Type_Definition *x, Ast_Type_Definition *y)
{
    if (x == y) return true;

    if (x->pointer_to && y->pointer_to) return pointer_types_are_equal(x, y);

    if (x->array_element_type && y->array_element_type) {
        if (x->array_length != y->array_length) return false;
        return types_are_equal(x->array_element_type, y->array_element_type);
    }

    if (x->lambda_return_type && y->lambda_return_type) {
        if (!types_are_equal(x->lambda_return_type, y->lambda_return_type)) return false;
        if (arrlenu(x->lambda_argument_types) != arrlenu(y->lambda_argument_types)) return false;
        For (x->lambda_argument_types) {
            if (!types_are_equal(x->lambda_argument_types[it], y->lambda_argument_types[it])) return false;
        }
        return true;
    }

    // All other types, such as structures and enumerations, can only be compared by pointer.
    // We do *NOT* do any duck typing or other functional programming strangeness. If you want
    // "duck" typing, you can use compile-time polymorphism or metaprogramming.

    return false;
}

#define RED   "\x1B[31m"
#define GRN   "\x1B[32m"
#define YEL   "\x1B[33m"
#define BLU   "\x1B[34m"
#define MAG   "\x1B[35m"
#define CYN   "\x1B[36m"
#define WHT   "\x1B[37m"
#define RESET "\x1B[0m"
#define TAB   "    "

void report_error(Workspace *workspace, Source_Location loc, const char *format, ...)
{
    va_list args;
    va_start(args, format);

    if (loc.l1 < 0) loc.l1 = loc.l0;
    if (loc.c1 < 0) loc.c1 = loc.c0;

    Source_File file = workspace->files[loc.fid];

    // Display the error message.
    fprintf(stderr, Loc_Fmt": Error: ", SV_Arg(file.path), Loc_Arg(loc));
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n\n");

    // TODO: I want to "normalize" the indentation.
    // When we add lines to the parser, we can add it
    // after it has been trimmed to remove leading spaces.
    // However, when printing the previous line, if they differ
    // in indentation I want to show that somehow.
    // Basically, if we are like 10 scopes deep in a function,
    // I want to only print indentation 1 level deeper or shallower
    // than the current line, so that when printing diagnostics we
    // don't end up printing like 50 spaces.

    int ln = loc.l0;

    // Display the previous line if it exists.
    String_View prev = (ln > 0) ? file.lines[ln-1] : SV_NULL;
    String_View line = file.lines[ln];

    if (prev.count > 1) {
        size_t count = Min(size_t, prev.count, line.count);
        size_t n = 0;
        while (n < count && prev.data[n] == line.data[n] && isspace(prev.data[n])) {
            n += 1;
        }
        sv_chop_left(&prev, n);
        sv_chop_left(&line, n);

        fprintf(stderr, TAB CYN SV_Fmt RESET, SV_Arg(prev));

        loc.c0 -= n;
        loc.c1 -= n;
    } else {
        size_t n = 0;
        while (n < line.count && isspace(line.data[n])) {
            n += 1;
        }
        sv_chop_left(&line, n);
        loc.c0 -= n;
        loc.c1 -= n;
    }

    // Highlight the token in red.

    fprintf(stderr, TAB CYN SV_Fmt, loc.c0, line.data);
    fprintf(stderr,     RED SV_Fmt, loc.c1 - loc.c0, line.data + loc.c0);
    fprintf(stderr,     CYN SV_Fmt, (int)line.count - loc.c1, line.data + loc.c1);
    fprintf(stderr, "\n" RESET);

    va_end(args);
    exit(1);
}
