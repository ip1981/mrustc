/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * parse/root.cpp
 * - Parsing at the module level (highest-level parsing)
 *
 * Entrypoint:
 * - Parse_Crate : Handles crate attrbutes, and passes on to Parse_ModRoot
 * - Parse_ModRoot
 */
#include <ast/ast.hpp>
#include <ast/crate.hpp>
#include "parseerror.hpp"
#include "common.hpp"
#include <cassert>
#include <hir/hir.hpp>  // ABI_RUST - TODO: Move elsewhere?
#include <expand/cfg.hpp>   // check_cfg - for `mod nonexistant;`
#include <fstream>  // Used by directory path
#include "lex.hpp"  // New file lexer
#include <parse/interpolated_fragment.hpp>
#include <ast/expr.hpp>
#include <macro_rules/macro_rules.hpp>
#include <path.h>

template<typename T>
Spanned<T> get_spanned(TokenStream& lex, ::std::function<T()> f) {
    auto ps = lex.start_span();
    auto v = f();
    return Spanned<T> {
        lex.end_span(ps),
        mv$(v)
        };
}
#define GET_SPANNED(type, lex, val) get_spanned< type >(lex, [&](){ return val; })

// Check the next two tokens
#define LOOKAHEAD2(lex, tok1, tok2) ((lex).lookahead(0) == (tok1) && (lex).lookahead(1) == (tok2))

::helpers::path dirname(::std::string input) {
    while( input.size() > 0 && input.back() != '/' && input.back() != '\\' ) {
        input.pop_back();
    }
    return input;
}

AST::AttributeList Parse_ItemAttrs(TokenStream& lex);
void Parse_ParentAttrs(TokenStream& lex, AST::AttributeList& out);
AST::Attribute  Parse_MetaItem(TokenStream& lex);
void Parse_ModRoot(TokenStream& lex, AST::Module& mod, AST::AttributeList& mod_attrs);
bool Parse_MacroInvocation_Opt(TokenStream& lex,  AST::MacroInvocation& out_inv);

::AST::Visibility Parse_Publicity(TokenStream& lex, bool allow_restricted/*=true*/)
{
    Token   tok;
    if( LOOK_AHEAD(lex) == TOK_INTERPOLATED_VIS )
    {
        GET_TOK(tok, lex);
        return tok.take_frag_vis();
    }
    if( LOOK_AHEAD(lex) == TOK_RWORD_CRATE )
    {
        // TODO: Return a path that indicates the entire current crate
        GET_TOK(tok, lex);
        return true;
    }
    if( LOOK_AHEAD(lex) == TOK_RWORD_PUB )
    {
        GET_TOK(tok, lex);
        if( LOOK_AHEAD(lex) == TOK_PAREN_OPEN )
        {
            // HACK: tuple structs have a parsing ambiguity around `pub (self::Type,)`
            if( !allow_restricted )
            {
                if( lex.lookahead(1) == TOK_RWORD_IN )
                    ;
                else if( lex.lookahead(1) == TOK_RWORD_CRATE && lex.lookahead(2) == TOK_PAREN_CLOSE )
                    ;
                else if( lex.lookahead(1) == TOK_RWORD_SUPER && lex.lookahead(2) == TOK_PAREN_CLOSE )
                    ;
                else if( lex.lookahead(1) == TOK_RWORD_SELF && lex.lookahead(2) == TOK_PAREN_CLOSE )
                    ;
                else
                    return true;
            }
            auto    path = AST::Path("", {});
            // Restricted publicity.
            GET_TOK(tok, lex);  // '('

            switch(GET_TOK(tok, lex))
            {
            case TOK_RWORD_CRATE:
                // Crate visibility
                break;
            case TOK_RWORD_SELF:
                // Private!
                path = AST::Path( lex.parse_state().get_current_mod().path() );
                break;
            case TOK_RWORD_SUPER:
                path = AST::Path( lex.parse_state().get_current_mod().path() );
                path.nodes().pop_back();
                while( lex.lookahead(0) == TOK_DOUBLE_COLON && lex.lookahead(1) == TOK_RWORD_SUPER )
                {
                    GET_TOK(tok, lex);
                    GET_TOK(tok, lex);
                    path.nodes().pop_back();
                }
                if( lex.lookahead(0) != TOK_DOUBLE_COLON )
                    break;
                GET_TOK(tok, lex);
                GET_CHECK_TOK(tok, lex, TOK_IDENT);
            case TOK_RWORD_IN:
                GET_CHECK_TOK(tok, lex, TOK_IDENT);
                path.nodes().push_back( AST::PathNode(tok.istr()) );
                while( LOOK_AHEAD(lex) == TOK_DOUBLE_COLON )
                {
                    GET_TOK(tok, lex);
                    GET_CHECK_TOK(tok, lex, TOK_IDENT);
                    path.nodes().push_back( AST::PathNode(tok.istr()) );
                }
                break;
            default:
                throw ParseError::Unexpected(lex, tok);
            }
            GET_CHECK_TOK(tok, lex, TOK_PAREN_CLOSE);
        }
        return true;
    }
    else
    {
        return false;
    }
}

::AST::HigherRankedBounds Parse_HRB(TokenStream& lex)
{
    TRACE_FUNCTION;
    Token   tok;

    ::AST::HigherRankedBounds   rv;
    GET_CHECK_TOK(tok, lex, TOK_LT);
    do {
        // Support empty lists and comma-terminated lists
        if( lex.lookahead(0) == TOK_GT ) {
            GET_TOK(tok, lex);
            break;
        }
        auto attrs = Parse_ItemAttrs(lex);

        switch(GET_TOK(tok, lex))
        {
        case TOK_LIFETIME:
            rv.m_lifetimes.push_back(::AST::LifetimeParam(lex.point_span(), ::std::move(attrs), lex.get_ident(mv$(tok))));
            break;
        default:
            throw ParseError::Unexpected(lex, tok, Token(TOK_LIFETIME));
        }
    } while( GET_TOK(tok, lex) == TOK_COMMA );
    CHECK_TOK(tok, TOK_GT);
    return rv;
}
::AST::HigherRankedBounds Parse_HRB_Opt(TokenStream& lex)
{
    if( lex.lookahead(0) == TOK_RWORD_FOR )
    {
        lex.getToken(); // Consume
        return Parse_HRB(lex);
    }
    else
    {
        return ::AST::HigherRankedBounds();
    }
}

namespace {
    AST::LifetimeRef get_LifetimeRef(TokenStream& lex, Token tok)
    {
        CHECK_TOK(tok, TOK_LIFETIME);
        return AST::LifetimeRef(/*lex.point_span(), */lex.get_ident(mv$(tok)));
    }
}
/// Parse type parameters in a definition
void Parse_TypeBound(TokenStream& lex, AST::GenericParams& ret, TypeRef checked_type, AST::HigherRankedBounds outer_hrbs = {})
{
    TRACE_FUNCTION;
    Token tok;

    do
    {
        // If an item terminator is seen (end of item, start of body, list separator), return early.
        //if( LOOK_AHEAD(lex) == TOK_SEMICOLON || LOOK_AHEAD(lex) == TOK_COMMA )
        //{
        //    return;
        //}

        if(GET_TOK(tok, lex) == TOK_LIFETIME) {
            ret.add_bound(AST::GenericBound::make_TypeLifetime( {
                checked_type.clone(), get_LifetimeRef(lex, mv$(tok))
                } ));
        }
        else if( tok.type() == TOK_QMARK ) {
            auto hrbs = Parse_HRB_Opt(lex);
            (void)hrbs; // The only valid ?Trait is Sized, which doesn't have any generics
            ret.add_bound(AST::GenericBound::make_MaybeTrait( {
                checked_type.clone(), Parse_Path(lex, PATH_GENERIC_TYPE)
                } ));
        }
        else {
            ::AST::HigherRankedBounds inner_hrls;
            if( tok.type() == TOK_RWORD_FOR )
            {
                inner_hrls = Parse_HRB(lex);
            }
            else {
                PUTBACK(tok, lex);
            }
            auto trait_path = Parse_Path(lex, PATH_GENERIC_TYPE);

            auto this_outer_hrbs = (lex.lookahead(0) == TOK_PLUS ? AST::HigherRankedBounds(outer_hrbs) : mv$(outer_hrbs));
            ret.add_bound( AST::GenericBound::make_IsTrait({
                mv$(this_outer_hrbs), checked_type.clone(), mv$(inner_hrls), mv$(trait_path)
                }) );
        }
    } while( GET_TOK(tok, lex) == TOK_PLUS );
    PUTBACK(tok, lex);
}

/// Parse type parameters within '<' and '>' (definition)
AST::GenericParams Parse_GenericParams(TokenStream& lex)
{
    TRACE_FUNCTION;

    AST::GenericParams ret;
    Token tok;
    do {
        if( GET_TOK(tok, lex) == TOK_GT ) {
            break ;
        }

        PUTBACK(tok, lex);
        auto attrs = Parse_ItemAttrs(lex);

        GET_TOK(tok, lex);
        if( tok.type() == TOK_IDENT )
        {
            auto param_name = tok.istr();
            ret.add_ty_param( AST::TypeParam( lex.point_span(), ::std::move(attrs), param_name ) );

            auto param_ty = TypeRef(lex.point_span(), param_name);
            if( GET_TOK(tok, lex) == TOK_COLON )
            {
                Parse_TypeBound(lex, ret, mv$(param_ty));
                GET_TOK(tok, lex);
            }

            if( tok.type() == TOK_EQUAL )
            {
                ret.ty_params().back().setDefault( Parse_Type(lex) );
                GET_TOK(tok, lex);
            }
        }
        else if( tok.type() == TOK_LIFETIME )
        {
            auto param_name = tok.istr();
            auto ref = get_LifetimeRef(lex, mv$(tok));
            ret.add_lft_param(::AST::LifetimeParam(lex.point_span(), ::std::move(attrs), Ident(lex.getHygiene(), param_name) ));
            if( GET_TOK(tok, lex) == TOK_COLON )
            {
                do {
                    GET_CHECK_TOK(tok, lex, TOK_LIFETIME);
                    ret.add_bound(AST::GenericBound::make_Lifetime({ AST::LifetimeRef(ref), get_LifetimeRef(lex, mv$(tok)) }));
                } while( GET_TOK(tok, lex) == TOK_PLUS );
            }
        }
        else
        {
            throw ParseError::Unexpected(lex, tok, {TOK_IDENT, TOK_LIFETIME});
        }
    } while( tok.type() == TOK_COMMA );
    PUTBACK(tok, lex);
    return ret;
}


/// Parse the contents of a 'where' clause
void Parse_WhereClause(TokenStream& lex, AST::GenericParams& params)
{
    TRACE_FUNCTION;
    Token   tok;

    do {
        GET_TOK(tok, lex);
        if( tok.type() == TOK_BRACE_OPEN ) {
            break;
        }

        if( tok.type() == TOK_LIFETIME )
        {
            auto lhs = get_LifetimeRef(lex, mv$(tok));
            GET_CHECK_TOK(tok, lex, TOK_COLON);
            do {
                GET_CHECK_TOK(tok, lex, TOK_LIFETIME);
                auto rhs = get_LifetimeRef(lex, mv$(tok));
                params.add_bound( AST::GenericBound::make_Lifetime({lhs, rhs}) );
            } while( GET_TOK(tok, lex) == TOK_PLUS );
            PUTBACK(tok, lex);
        }
        // Higher-ranked types/lifetimes
        else if( tok.type() == TOK_RWORD_FOR )
        {
            auto hrbs = Parse_HRB(lex);

            TypeRef type = Parse_Type(lex);
            GET_CHECK_TOK(tok, lex, TOK_COLON);
            Parse_TypeBound(lex,params, mv$(type), mv$(hrbs));
        }
        else
        {
            PUTBACK(tok, lex);
            TypeRef type = Parse_Type(lex);
            GET_CHECK_TOK(tok, lex, TOK_COLON);
            Parse_TypeBound(lex, params, mv$(type));
        }
    } while( GET_TOK(tok, lex) == TOK_COMMA );
    PUTBACK(tok, lex);
}

// Parse a single function argument
::std::pair< AST::Pattern, TypeRef> Parse_Function_Arg(TokenStream& lex, bool expect_named)
{
    TRACE_FUNCTION_F("expect_named = " << expect_named);
    Token   tok;

    AST::Pattern pat;

    // If any of the following
    // - Expecting a named parameter (i.e. defining a function in root or impl)
    // - Next token is an underscore (only valid as a pattern here)
    // - Next token is 'mut' (a mutable parameter slot)
    // - Next two are <ident> ':' (a trivial named parameter)
    // NOTE: When not expecting a named param, destructuring patterns are not allowed
    if( expect_named
      || LOOK_AHEAD(lex) == TOK_UNDERSCORE
      || LOOK_AHEAD(lex) == TOK_RWORD_MUT
      || (LOOK_AHEAD(lex) == TOK_IDENT && lex.lookahead(1) == TOK_COLON)
      )
    {
        // Function args can't be refuted
        pat = Parse_Pattern(lex, false);
        GET_CHECK_TOK(tok, lex, TOK_COLON);
    }

    TypeRef type = Parse_Type(lex);


    return ::std::make_pair( ::std::move(pat), ::std::move(type) );
}

/// Parse a function definition (after the 'fn <name>')
AST::Function Parse_FunctionDef(TokenStream& lex, ::std::string abi, bool allow_self, bool can_be_prototype,  bool is_unsafe, bool is_const)
{
    TRACE_FUNCTION;
    ProtoSpan   ps = lex.start_span();

    Token   tok;

    // Parameters
    AST::GenericParams params;
    if( GET_TOK(tok, lex) == TOK_LT )
    {
        params = Parse_GenericParams(lex);
        GET_CHECK_TOK(tok, lex, TOK_GT);
    }
    else {
        PUTBACK(tok, lex);
    }

    AST::Function::Arglist  args;

    GET_CHECK_TOK(tok, lex, TOK_PAREN_OPEN);
    GET_TOK(tok, lex);

    // Handle self
    if( tok.type() == TOK_AMP )
    {
        // By-reference method?

        unsigned int ofs = 0;
        // Handle a lifetime parameter name
        if( lex.lookahead(0) == TOK_LIFETIME )
            ofs ++;

        if( lex.lookahead(ofs) == TOK_RWORD_SELF || (lex.lookahead(ofs) == TOK_RWORD_MUT && lex.lookahead(ofs+1) == TOK_RWORD_SELF) )
        {
            auto ps = lex.start_span();
            AST::LifetimeRef lifetime;
            if( GET_TOK(tok, lex) == TOK_LIFETIME ) {
                lifetime = get_LifetimeRef(lex, mv$(tok));
                GET_TOK(tok, lex);
            }

            bool is_mut = false;
            if( tok.type() == TOK_RWORD_MUT )
            {
                is_mut = true;
                GET_TOK(tok, lex);
            }
            CHECK_TOK(tok, TOK_RWORD_SELF);
            auto sp = lex.end_span(ps);
            args.push_back( ::std::make_pair( AST::Pattern(AST::Pattern::TagBind(), sp, "self"), TypeRef(TypeRef::TagReference(), sp, ::std::move(lifetime), is_mut, TypeRef(sp, "Self", 0xFFFF))) );
            if( allow_self == false )
                throw ParseError::Generic(lex, "Self binding not expected");

            // Prime tok for next step
            GET_TOK(tok, lex);
        }
        else
        {
            // Unbound method
        }
    }
    else if( tok.type() == TOK_RWORD_MUT )
    {
        if( LOOK_AHEAD(lex) == TOK_RWORD_SELF )
        {
            GET_TOK(tok, lex);
            if( allow_self == false )
                throw ParseError::Generic(lex, "Self binding not expected");
            auto binding_sp = lex.end_span(ps);
            TypeRef ty = TypeRef( lex.point_span(), "Self", 0xFFFF );
            if( GET_TOK(tok, lex) == TOK_COLON ) {
                // Typed mut self
                ty = Parse_Type(lex);
            }
            else {
                PUTBACK(tok, lex);
            }
            args.push_back( ::std::make_pair( AST::Pattern(AST::Pattern::TagBind(), binding_sp, "self"), mv$(ty)) );
            GET_TOK(tok, lex);
        }
    }
    else if( tok.type() == TOK_RWORD_SELF )
    {
        // By-value method
        if( allow_self == false )
            throw ParseError::Generic(lex, "Self binding not expected");
        auto binding_sp = lex.end_span(ps);
        TypeRef ty = TypeRef( lex.point_span(), "Self", 0xFFFF );
        if( GET_TOK(tok, lex) == TOK_COLON ) {
            // Typed mut self
            ty = Parse_Type(lex);
        }
        else {
            PUTBACK(tok, lex);
        }
        args.push_back( ::std::make_pair( AST::Pattern(AST::Pattern::TagBind(), binding_sp, "self"), mv$(ty)) );
        GET_TOK(tok, lex);
    }
    else
    {
        // Unbound method
    }

    bool    is_variadic = false;
    if( tok.type() != TOK_PAREN_CLOSE )
    {
        // Comma after self
        if( args.size() )
        {
            CHECK_TOK(tok, TOK_COMMA);
        }
        else {
            PUTBACK(tok, lex);
        }

        // Argument list
        do {
            if( LOOK_AHEAD(lex) == TOK_PAREN_CLOSE ) {
                GET_TOK(tok, lex);
                break;
            }
            if( LOOK_AHEAD(lex) == TOK_TRIPLE_DOT ) {
                GET_TOK(tok, lex);
                is_variadic = true;
                GET_TOK(tok, lex);
                break;
            }
            args.push_back( Parse_Function_Arg(lex, !can_be_prototype) );
        } while( GET_TOK(tok, lex) == TOK_COMMA );
        CHECK_TOK(tok, TOK_PAREN_CLOSE);
    }
    else {
        // Eat 'tok', negative comparison
    }

    TypeRef ret_type = TypeRef(TypeRef::TagUnit(), Span(tok.get_pos()));
    if( GET_TOK(tok, lex) == TOK_THINARROW )
    {
        // Return type
        ret_type = Parse_Type(lex);
    }
    else
    {
        PUTBACK(tok, lex);
    }

    if( GET_TOK(tok, lex) == TOK_RWORD_WHERE )
    {
        Parse_WhereClause(lex, params);
    }
    else {
        PUTBACK(tok, lex);
    }

    return AST::Function(lex.end_span(ps), mv$(params), mv$(abi), is_unsafe, is_const, is_variadic, mv$(ret_type), mv$(args));
}

AST::Function Parse_FunctionDefWithCode(TokenStream& lex, ::std::string abi, bool allow_self, bool is_unsafe, bool is_const)
{
    Token   tok;
    auto ret = Parse_FunctionDef(lex, abi, allow_self, false,  is_unsafe, is_const);
    GET_CHECK_TOK(tok, lex, TOK_BRACE_OPEN);
    // Enter a new hygine scope for the function (TODO: Should this be in Parse_ExprBlock?)
    lex.push_hygine();
    PUTBACK(tok, lex);
    ret.set_code( Parse_ExprBlock(lex) );
    lex.pop_hygine();
    return ret;
}

AST::TypeAlias Parse_TypeAlias(TokenStream& lex)
{
    TRACE_FUNCTION;

    Token   tok;

    // Params
    AST::GenericParams params;
    if( GET_TOK(tok, lex) == TOK_LT )
    {
        params = Parse_GenericParams(lex);
        GET_CHECK_TOK(tok, lex, TOK_GT);
        GET_TOK(tok, lex);
    }

    if( tok.type() == TOK_RWORD_WHERE )
    {
        Parse_WhereClause(lex, params);
        GET_TOK(tok, lex);
    }
    CHECK_TOK(tok, TOK_EQUAL);

    // Type
    TypeRef type = Parse_Type(lex);
    GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);

    return AST::TypeAlias( ::std::move(params), ::std::move(type) );
}

AST::Struct Parse_Struct(TokenStream& lex, const AST::AttributeList& meta_items)
{
    TRACE_FUNCTION;

    Token   tok;

    tok = lex.getToken();
    AST::GenericParams params;
    if( tok.type() == TOK_LT )
    {
        params = Parse_GenericParams(lex);
        GET_CHECK_TOK(tok, lex, TOK_GT);
        if(GET_TOK(tok, lex) == TOK_RWORD_WHERE)
        {
            Parse_WhereClause(lex, params);
            tok = lex.getToken();
        }
    }
    if(tok.type() == TOK_PAREN_OPEN)
    {
        // Tuple structs
        ::std::vector<AST::TupleItem>  refs;
        while(lex.lookahead(0) != TOK_PAREN_CLOSE)
        {
            auto item_attrs = Parse_ItemAttrs(lex);
            SET_ATTRS(lex, item_attrs);

            bool    is_pub = Parse_Publicity(lex, /*allow_restricted=*/false);  // HACK: Disable `pub(restricted)` syntax in tuple structs, due to ambiguity

            refs.push_back( AST::TupleItem( mv$(item_attrs), is_pub, Parse_Type(lex) ) );
            if( GET_TOK(tok, lex) != TOK_COMMA ) {
                PUTBACK(tok, lex);
                break;
            }
        }
        GET_CHECK_TOK(tok, lex, TOK_PAREN_CLOSE);

        if(LOOK_AHEAD(lex) == TOK_RWORD_WHERE)
        {
            GET_TOK(tok, lex);
            Parse_WhereClause(lex, params);
        }
        GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
        //if( refs.size() == 0 )
        //    WARNING( , W000, "Use 'struct Name;' instead of 'struct Name();' ... ning-nong");
        return AST::Struct(mv$(params), mv$(refs));
    }
    else if(tok.type() == TOK_SEMICOLON)
    {
        // Unit-like struct
        return AST::Struct(mv$(params));
    }
    else if(tok.type() == TOK_BRACE_OPEN)
    {
        ::std::vector<AST::StructItem>  items;
        while( GET_TOK(tok, lex) != TOK_BRACE_CLOSE )
        {
            PUTBACK(tok, lex);

            auto item_attrs = Parse_ItemAttrs(lex);
            SET_ATTRS(lex, item_attrs);

            bool is_pub = Parse_Publicity(lex);

            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            auto name = tok.istr();
            GET_CHECK_TOK(tok, lex, TOK_COLON);
            TypeRef type = Parse_Type(lex);

            items.push_back( AST::StructItem( mv$(item_attrs), is_pub, mv$(name), mv$(type) ) );
            if(GET_TOK(tok, lex) == TOK_BRACE_CLOSE)
                break;
            CHECK_TOK(tok, TOK_COMMA);
        }
        //if( items.size() == 0 )
        //    WARNING( , W000, "Use 'struct Name;' instead of 'struct Nam { };' ... ning-nong");
        return AST::Struct(mv$(params), mv$(items));
    }
    else
    {
        throw ParseError::Unexpected(lex, tok);
    }
}

AST::Named<AST::Item> Parse_Trait_Item(TokenStream& lex)
{
    Token   tok;

    auto item_attrs = Parse_ItemAttrs(lex);
    SET_ATTRS(lex, item_attrs);

    auto ps = lex.start_span();
    {
        ::AST::MacroInvocation  inv;
        if( Parse_MacroInvocation_Opt(lex, inv) )
        {
            return AST::Named<AST::Item>( lex.end_span(ps), mv$(item_attrs), false, "", AST::Item(mv$(inv)) );
        }
    }

    GET_TOK(tok, lex);
    bool is_specialisable = false;
    if( tok.type() == TOK_IDENT && tok.istr() == "default" ) {
        is_specialisable = true;
        GET_TOK(tok, lex);
    }
    // TODO: Mark specialisation
    (void)is_specialisable;

    bool fn_is_const = false;
    bool fn_is_unsafe = false;
    ::std::string   abi = ABI_RUST;

    RcString    name;
    ::AST::Item   rv;
    switch(tok.type())
    {
    case TOK_RWORD_STATIC: {
        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        name = tok.istr();
        GET_CHECK_TOK(tok, lex, TOK_COLON);
        auto ty = Parse_Type(lex);
        GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);

        ::AST::Expr val;
        if(GET_TOK(tok, lex) == TOK_EQUAL) {
            val = Parse_Expr(lex);
            GET_TOK(tok, lex);
        }
        CHECK_TOK(tok, TOK_SEMICOLON);

        rv = ::AST::Static(::AST::Static::STATIC, mv$(ty), val);
        break; }
    case TOK_RWORD_CONST: {
        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        name = tok.istr();
        GET_CHECK_TOK(tok, lex, TOK_COLON);
        auto ty = Parse_Type(lex);

        ::AST::Expr val;
        if(GET_TOK(tok, lex) == TOK_EQUAL) {
            val = Parse_Expr(lex);
            GET_TOK(tok, lex);
        }
        CHECK_TOK(tok, TOK_SEMICOLON);

        rv = ::AST::Static(AST::Static::CONST, mv$(ty), val);
        break; }
    // Associated type
    case TOK_RWORD_TYPE: {
        auto atype_params = ::AST::GenericParams { };
        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        name = tok.istr();
        if( GET_TOK(tok, lex) == TOK_COLON )
        {
            // Bounded associated type
            Parse_TypeBound(lex, atype_params, TypeRef(lex.point_span(), "Self", 0xFFFF));
            GET_TOK(tok, lex);
        }
        if( tok.type() == TOK_RWORD_WHERE ) {
            throw ParseError::Todo(lex, "Where clause on associated type");
        }

        TypeRef default_type = TypeRef( lex.point_span() );
        if( tok.type() == TOK_EQUAL ) {
            default_type = Parse_Type(lex);
            GET_TOK(tok, lex);
        }

        CHECK_TOK(tok, TOK_SEMICOLON);
        rv = ::AST::TypeAlias( mv$(atype_params), mv$(default_type) );
        break; }

    // Functions (possibly unsafe)
    case TOK_RWORD_UNSAFE:
        fn_is_unsafe = true;
        if( GET_TOK(tok, lex) == TOK_RWORD_EXTERN )
    case TOK_RWORD_EXTERN:
        {
            abi = "C";
            if( GET_TOK(tok, lex) == TOK_STRING )
                abi = tok.str();
            else
                PUTBACK(tok, lex);

            GET_TOK(tok, lex);
        }
        CHECK_TOK(tok, TOK_RWORD_FN);
    case TOK_RWORD_FN: {
        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        name = tok.istr();
        // Self allowed, prototype-form allowed (optional names and no code)
        auto fcn = Parse_FunctionDef(lex, abi, true, true,  fn_is_unsafe, fn_is_const);
        if( GET_TOK(tok, lex) == TOK_BRACE_OPEN )
        {
            PUTBACK(tok, lex);
            // Enter a new hygine scope for the function body. (TODO: Should this be in Parse_ExprBlock?)
            lex.push_hygine();
            fcn.set_code( Parse_ExprBlock(lex) );
            lex.pop_hygine();
        }
        else if( tok.type() == TOK_SEMICOLON )
        {
            // Accept it
        }
        else
        {
            throw ParseError::Unexpected(lex, tok);
        }
        rv = ::std::move(fcn);
        break; }
    default:
        throw ParseError::Unexpected(lex, tok);
    }

    return ::AST::Named<::AST::Item>( lex.end_span(ps), mv$(item_attrs), true, mv$(name), mv$(rv) );
}

AST::Trait Parse_TraitDef(TokenStream& lex, const AST::AttributeList& meta_items)
{
    TRACE_FUNCTION;

    Token   tok;

    AST::GenericParams params;
    if( GET_TOK(tok, lex) == TOK_LT )
    {
        params = Parse_GenericParams(lex);
        GET_CHECK_TOK(tok, lex, TOK_GT);
        tok = lex.getToken();
    }

    // Trait bounds "trait Trait : 'lifetime + OtherTrait + OtherTrait2"
    ::std::vector<Spanned<Type_TraitPath> >    supertraits;
    if(tok.type() == TOK_COLON)
    {
        // TODO: Just add these as `where Self: <foo>` (would that break typecheck?)
        do {
            if( GET_TOK(tok, lex) == TOK_LIFETIME ) {
                params.add_bound(::AST::GenericBound::make_TypeLifetime({ TypeRef(lex.point_span(), "Self"), ::AST::LifetimeRef(lex.get_ident(tok)) }));
            }
            else if( tok.type() == TOK_BRACE_OPEN ) {
                break;
            }
            else {
                PUTBACK(tok, lex);
                auto hrbs = Parse_HRB_Opt(lex);
                supertraits.push_back( GET_SPANNED(Type_TraitPath, lex, (Type_TraitPath{ mv$(hrbs), Parse_Path(lex, PATH_GENERIC_TYPE) })) );
            }
        } while( GET_TOK(tok, lex) == TOK_PLUS );
    }

    if(tok.type() == TOK_RWORD_WHERE)
    {
        //if( params.ty_params().size() == 0 )
        //    throw ParseError::Generic("Where clause with no generic params");
        Parse_WhereClause(lex, params);
        tok = lex.getToken();
    }


    AST::Trait trait( mv$(params), mv$(supertraits) );

    CHECK_TOK(tok, TOK_BRACE_OPEN);
    while( GET_TOK(tok, lex) != TOK_BRACE_CLOSE )
    {
        PUTBACK(tok, lex);

        trait.items().push_back( Parse_Trait_Item(lex) );
    }

    return trait;
}

AST::Enum Parse_EnumDef(TokenStream& lex, const AST::AttributeList& meta_items)
{
    TRACE_FUNCTION;

    Token   tok;

    tok = lex.getToken();
    // Type params supporting "where"
    AST::GenericParams params;
    if( tok.type() == TOK_LT )
    {
        params = Parse_GenericParams(lex);
        GET_CHECK_TOK(tok, lex, TOK_GT);
        if(GET_TOK(tok, lex) == TOK_RWORD_WHERE)
        {
            Parse_WhereClause(lex, params);
            tok = lex.getToken();
        }
    }

    // Body
    CHECK_TOK(tok, TOK_BRACE_OPEN);
    ::std::vector<AST::EnumVariant>   variants;
    while( GET_TOK(tok, lex) != TOK_BRACE_CLOSE )
    {
        auto sp = lex.start_span();
        PUTBACK(tok, lex);

        auto item_attrs = Parse_ItemAttrs(lex);
        SET_ATTRS(lex, item_attrs);

        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        auto name = tok.istr();
        // Tuple-like variants
        if( GET_TOK(tok, lex) == TOK_PAREN_OPEN )
        {
            ::std::vector<TypeRef>  types;
            // Get type list
            do
            {
                if(LOOK_AHEAD(lex) == TOK_PAREN_CLOSE)
                {
                    GET_TOK(tok, lex);
                    break;
                }

                auto field_attrs = Parse_ItemAttrs(lex);
                (void)field_attrs;  // TODO: Store field_attrs

                types.push_back( Parse_Type(lex) );
            } while( GET_TOK(tok, lex) == TOK_COMMA );
            CHECK_TOK(tok, TOK_PAREN_CLOSE);
            GET_TOK(tok, lex);
            variants.push_back( AST::EnumVariant(mv$(item_attrs), mv$(name), mv$(types)) );
        }
        // Struct-like variants
        else if( tok.type() == TOK_BRACE_OPEN )
        {
            ::std::vector<::AST::StructItem>   fields;
            do
            {
                if(LOOK_AHEAD(lex) == TOK_BRACE_CLOSE)
                {
                    GET_TOK(tok, lex);
                    break;
                }

                auto field_attrs = Parse_ItemAttrs(lex);

                GET_CHECK_TOK(tok, lex, TOK_IDENT);
                auto name = tok.istr();
                GET_CHECK_TOK(tok, lex, TOK_COLON);
                auto ty = Parse_Type(lex);
                fields.push_back( ::AST::StructItem(mv$(field_attrs), true, mv$(name), mv$(ty)) );
            } while( GET_TOK(tok, lex) == TOK_COMMA );
            CHECK_TOK(tok, TOK_BRACE_CLOSE);
            GET_TOK(tok, lex);

            variants.push_back( AST::EnumVariant(mv$(item_attrs), mv$(name), mv$(fields)) );
        }
        // Value variants
        else if( tok.type() == TOK_EQUAL )
        {
            auto node = Parse_Expr(lex);
            variants.push_back( AST::EnumVariant(mv$(item_attrs), mv$(name), mv$(node)) );
            GET_TOK(tok, lex);
        }
        // Unit variants
        else
        {
            variants.push_back( AST::EnumVariant(mv$(item_attrs), mv$(name), ::AST::Expr()) );
        }

        if( tok.type() != TOK_COMMA )
            break;
    }
    CHECK_TOK(tok, TOK_BRACE_CLOSE);


    return AST::Enum( mv$(params), mv$(variants) );
}

::AST::Union Parse_Union(TokenStream& lex, AST::AttributeList& meta_items)
{
    Token   tok;

    TRACE_FUNCTION;

    AST::GenericParams params;
    if( GET_TOK(tok, lex) == TOK_LT )
    {
        params = Parse_GenericParams(lex);
        GET_CHECK_TOK(tok, lex, TOK_GT);
        if(GET_TOK(tok, lex) == TOK_RWORD_WHERE)
        {
            Parse_WhereClause(lex, params);
            tok = lex.getToken();
        }
    }

    ::std::vector< ::AST::StructItem>   variants;

    CHECK_TOK(tok, TOK_BRACE_OPEN);
    do {
        if( LOOK_AHEAD(lex) == TOK_BRACE_CLOSE ) {
            GET_TOK(tok, lex);
            break ;
        }

        auto item_attrs = Parse_ItemAttrs(lex);
        SET_ATTRS(lex, item_attrs);

        bool is_pub = Parse_Publicity(lex);

        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        auto name = tok.istr();
        GET_CHECK_TOK(tok, lex, TOK_COLON);

        auto ty = Parse_Type(lex);

        variants.push_back( ::AST::StructItem( mv$(item_attrs), is_pub, mv$(name), mv$(ty) ) );

    } while( GET_TOK(tok, lex) == TOK_COMMA );
    CHECK_TOK(tok, TOK_BRACE_CLOSE);

    return ::AST::Union( mv$(params), mv$(variants) );
}

AST::AttributeList Parse_ItemAttrs(TokenStream& lex)
{
    AST::AttributeList  rv;
    Token   tok;
    while( lex.lookahead(0) == TOK_HASH )
    {
        GET_CHECK_TOK(tok, lex, TOK_HASH);
        GET_CHECK_TOK(tok, lex, TOK_SQUARE_OPEN);
        rv.push_back( Parse_MetaItem(lex) );
        GET_CHECK_TOK(tok, lex, TOK_SQUARE_CLOSE);
    }
    return rv;
}
void Parse_ParentAttrs(TokenStream& lex, AST::AttributeList& out)
{
    Token   tok;
    while( lex.lookahead(0) == TOK_HASH && lex.lookahead(1) == TOK_EXCLAM )
    {
        GET_CHECK_TOK(tok, lex, TOK_HASH);
        GET_CHECK_TOK(tok, lex, TOK_EXCLAM);
        GET_CHECK_TOK(tok, lex, TOK_SQUARE_OPEN);
        out.push_back( Parse_MetaItem(lex) );
        GET_CHECK_TOK(tok, lex, TOK_SQUARE_CLOSE);
    }
}
/// Parse a meta-item declaration (either #![ or #[)
AST::Attribute Parse_MetaItem(TokenStream& lex)
{
    TRACE_FUNCTION;
    Token tok;

    if( lex.lookahead(0) == TOK_INTERPOLATED_META ) {
        GET_TOK(tok, lex);
        return mv$(tok.frag_meta());
    }

    auto ps = lex.start_span();
    GET_TOK(tok, lex);

    switch(tok.type())
    {
    case TOK_IDENT:
        break;
    case TOK_INTEGER:
        if( TARGETVER_1_29 )
            return AST::Attribute(lex.end_span(ps), "", tok.to_str());
    default:
        throw ParseError::Unexpected(lex, tok, {TOK_IDENT, TOK_INTEGER});
    }

    auto name = tok.istr();
    switch(GET_TOK(tok, lex))
    {
    case TOK_EQUAL:
        switch(GET_TOK(tok, lex))
        {
        case TOK_STRING:
            return AST::Attribute(lex.end_span(ps), name, tok.str());
        case TOK_INTERPOLATED_EXPR: {
            auto n = tok.take_frag_node();
            void Expand_BareExpr(const AST::Crate& , const AST::Module&, ::std::unique_ptr<AST::ExprNode>& n);
            assert( lex.parse_state().crate );
            assert( lex.parse_state().module );
            Expand_BareExpr(*lex.parse_state().crate, *lex.parse_state().module, n);
            if( auto* v = dynamic_cast<::AST::ExprNode_String*>(&*n) )
            {
                return AST::Attribute(lex.end_span(ps), name, mv$(v->m_value));
            }
            else
            {
                // - Force an error.
                throw ParseError::Unexpected(lex, Token(InterpolatedFragment(InterpolatedFragment::EXPR, n.release())), TOK_STRING);
            }
            break; }
        case TOK_INTEGER:
            if( TARGETVER_1_29 )
                return AST::Attribute(lex.end_span(ps), name, tok.to_str());
        case TOK_IDENT:
            if( TARGETVER_1_29 )
                return AST::Attribute(lex.end_span(ps), name, tok.to_str());
        default:
            // - Force an error.
            CHECK_TOK(tok, TOK_STRING);
        }
        throw "";
    case TOK_PAREN_OPEN: {
        ::std::vector<AST::Attribute>    items;
        do {
            if(LOOK_AHEAD(lex) == TOK_PAREN_CLOSE) {
                GET_TOK(tok, lex);
                break;
            }
            items.push_back(Parse_MetaItem(lex));
        } while(GET_TOK(tok, lex) == TOK_COMMA);
        CHECK_TOK(tok, TOK_PAREN_CLOSE);
        return AST::Attribute(lex.end_span(ps), name, mv$(items)); }
    default:
        PUTBACK(tok, lex);
        return AST::Attribute(lex.end_span(ps), name);
    }
}

::AST::Item Parse_Impl(TokenStream& lex, AST::AttributeList attrs, bool is_unsafe=false)
{
    TRACE_FUNCTION;
    Token   tok;
    auto ps = lex.start_span();

    AST::GenericParams params;
    // 1. (optional) type parameters
    if( GET_TOK(tok, lex) == TOK_LT )
    {
        params = Parse_GenericParams(lex);
        GET_CHECK_TOK(tok, lex, TOK_GT);
    }
    else {
        PUTBACK(tok, lex);
    }
    // 2. Either a trait name (with type params), or the type to impl

    Spanned<AST::Path>   trait_path;

    // - Handle negative impls specially, which must be a trait
    // "impl !Trait for Type {}"
    if( GET_TOK(tok, lex) == TOK_EXCLAM )
    {
        trait_path = GET_SPANNED(::AST::Path, lex, Parse_Path(lex, PATH_GENERIC_TYPE));
        GET_CHECK_TOK(tok, lex, TOK_RWORD_FOR);
        auto impl_type = Parse_Type(lex, true);

        if( GET_TOK(tok, lex) == TOK_RWORD_WHERE )
        {
            Parse_WhereClause(lex, params);
            GET_TOK(tok, lex);
        }
        CHECK_TOK(tok, TOK_BRACE_OPEN);
        // negative impls can't have any content
        GET_CHECK_TOK(tok, lex, TOK_BRACE_CLOSE);

        return ::AST::Item::make_NegImpl(AST::ImplDef( mv$(attrs), mv$(params), mv$(trait_path), mv$(impl_type) ));
    }

    // - Don't care which at this stage
    PUTBACK(tok, lex);

    auto impl_type = Parse_Type(lex, true);

    if( GET_TOK(tok, lex) == TOK_RWORD_FOR )
    {
        // Trickery! All traits parse as valid types, so this works.
        if( !impl_type.is_path() )
            throw ParseError::Generic(lex, "Trait was not a path");
        trait_path = Spanned< AST::Path> {
            impl_type.span(),
            mv$(impl_type.path())
            };
        // Implementing a trait for another type, get the target type
        if( GET_TOK(tok, lex) == TOK_DOUBLE_DOT )
        {
            // Default impl
            impl_type = TypeRef(TypeRef::TagInvalid(), lex.point_span());
        }
        else
        {
            PUTBACK(tok, lex);
            impl_type = Parse_Type(lex, true);
        }
    }
    else {
        PUTBACK(tok, lex);
    }

    // Where clause
    if( GET_TOK(tok, lex) == TOK_RWORD_WHERE )
    {
        Parse_WhereClause(lex, params);
    }
    else {
        PUTBACK(tok, lex);
    }
    GET_CHECK_TOK(tok, lex, TOK_BRACE_OPEN);

    Parse_ParentAttrs(lex,  attrs);

    auto impl = AST::Impl(AST::ImplDef( mv$(attrs), mv$(params), mv$(trait_path), mv$(impl_type) ));

    // A sequence of method implementations
    while( lex.lookahead(0) != TOK_BRACE_CLOSE )
    {
        Parse_Impl_Item(lex, impl);
    }
    GET_CHECK_TOK(tok, lex, TOK_BRACE_CLOSE);

    return ::AST::Item::make_Impl( mv$(impl) );
}

void Parse_Impl_Item(TokenStream& lex, AST::Impl& impl)
{
    TRACE_FUNCTION;
    Token   tok;

    auto item_attrs = Parse_ItemAttrs(lex);
    SET_ATTRS(lex, item_attrs);

    {
        ::AST::MacroInvocation  inv;
        if( Parse_MacroInvocation_Opt(lex,  inv) )
        {
            impl.add_macro_invocation( mv$(inv) );
            impl.items().back().attrs = mv$(item_attrs);
            return ;
        }
    }

    auto ps = lex.start_span();

    bool is_public = Parse_Publicity(lex);
    GET_TOK(tok, lex);

    bool is_specialisable = false;
    if( tok.type() == TOK_IDENT && tok.istr() == "default" ) {
        is_specialisable = true;
        GET_TOK(tok, lex);
    }

    ::std::string   abi = ABI_RUST;
    bool fn_is_unsafe = false;
    bool fn_is_const = false;
    switch(tok.type())
    {
    case TOK_RWORD_TYPE: {
        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        auto name = tok.istr();
        GET_CHECK_TOK(tok, lex, TOK_EQUAL);
        auto ty = Parse_Type(lex);
        GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
        impl.add_type(lex.end_span(ps), mv$(item_attrs), is_public, is_specialisable, name, mv$(ty));
        break; }
    case TOK_RWORD_UNSAFE:
        fn_is_unsafe = true;
        GET_TOK(tok, lex);
        if( tok.type() == TOK_RWORD_CONST )
    case TOK_RWORD_CONST:
        {
            GET_TOK(tok, lex);
            if( tok.type() != TOK_RWORD_FN && tok.type() != TOK_RWORD_UNSAFE && !fn_is_unsafe )
            {
                CHECK_TOK(tok, TOK_IDENT);
                auto name = tok.istr();
                GET_CHECK_TOK(tok, lex, TOK_COLON);
                auto ty = Parse_Type(lex);
                GET_CHECK_TOK(tok, lex, TOK_EQUAL);
                auto val = Parse_Expr(lex);
                GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);

                auto i = ::AST::Static(AST::Static::CONST, mv$(ty), mv$(val));
                impl.add_static( lex.end_span(ps), mv$(item_attrs), is_public, is_specialisable, mv$(name),  mv$(i) );
                break ;
            }
            else if( tok.type() == TOK_RWORD_UNSAFE )
            {
                fn_is_unsafe = true;
                GET_CHECK_TOK(tok, lex, TOK_RWORD_FN);
            }
            fn_is_const = true;
        }
        if( tok.type() == TOK_RWORD_EXTERN )
        // FALL
    case TOK_RWORD_EXTERN:
        {
            abi = "C";
            if( GET_TOK(tok, lex) == TOK_STRING )
                abi = tok.str();
            else
                PUTBACK(tok, lex);

            GET_TOK(tok, lex);
        }
        CHECK_TOK(tok, TOK_RWORD_FN);
        // FALL
    case TOK_RWORD_FN: {
        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        // TODO: Hygine on function names? - Not in impl blocks?
        auto name = tok.istr();
        DEBUG("Function " << name);
        // - Self allowed, can't be prototype-form
        auto fcn = Parse_FunctionDefWithCode(lex, abi, true,  fn_is_unsafe, fn_is_const);
        impl.add_function(lex.end_span(ps), mv$(item_attrs), is_public, is_specialisable, mv$(name), mv$(fcn));
        break; }

    default:
        throw ParseError::Unexpected(lex, tok);
    }
}

AST::ExternBlock Parse_ExternBlock(TokenStream& lex, ::std::string abi, ::AST::AttributeList& block_attrs)
{
    TRACE_FUNCTION;
    Token   tok;

    Parse_ParentAttrs(lex,  block_attrs);

    AST::ExternBlock    rv { abi };

    while( GET_TOK(tok, lex) != TOK_BRACE_CLOSE )
    {
        PUTBACK(tok, lex);
        auto meta_items = Parse_ItemAttrs(lex);
        SET_ATTRS(lex, meta_items);

        auto ps = lex.start_span();

        bool is_public = Parse_Publicity(lex);
        switch( GET_TOK(tok, lex) )
        {
        case TOK_RWORD_FN: {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            auto name = tok.istr();
            // parse function as prototype
            // - no self, is prototype, is unsafe and not const
            auto i = ::AST::Item( Parse_FunctionDef(lex, abi, false, true,  true,false) );
            GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);

            rv.add_item( AST::Named<AST::Item> { lex.end_span(ps), mv$(meta_items), is_public, mv$(name), mv$(i) } );
            break; }
        case TOK_RWORD_STATIC: {
            bool is_mut = false;
            if( GET_TOK(tok, lex) == TOK_RWORD_MUT )
                is_mut = true;
            else
                PUTBACK(tok, lex);
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            auto name = tok.istr();
            GET_CHECK_TOK(tok, lex, TOK_COLON);
            auto type = Parse_Type(lex);
            GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);

            auto i = ::AST::Item(::AST::Static( (is_mut ? ::AST::Static::MUT : ::AST::Static::STATIC),  mv$(type), ::AST::Expr() ));
            rv.add_item( AST::Named<AST::Item> { lex.end_span(ps), mv$(meta_items), is_public,  mv$(name), mv$(i) } );
            break; }
        case TOK_RWORD_TYPE: {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            auto name = tok.istr();
            GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
            auto sp = lex.end_span(ps);
            //TODO(sp, "Extern type");
            auto i = ::AST::Item(::AST::TypeAlias( ::AST::GenericParams(), ::TypeRef(sp) ));
            rv.add_item( AST::Named<AST::Item> { mv$(sp), mv$(meta_items), is_public, mv$(name), mv$(i) } );
            break; }
        default:
            throw ParseError::Unexpected(lex, tok, {TOK_RWORD_FN, TOK_RWORD_STATIC, TOK_RWORD_TYPE});
        }
    }

    return rv;
}

/// Parse multiple items from a use "statement"
void Parse_Use_Inner(TokenStream& lex, ::std::vector<AST::UseItem::Ent>& entries, AST::Path& path)
{
    Token   tok;

    do
    {
        switch( GET_TOK(tok, lex) )
        {
        case TOK_IDENT:
            path.append( AST::PathNode( tok.istr(), {}) );
            break;
        case TOK_BRACE_OPEN:
            // Can't be an empty list
            if( LOOK_AHEAD(lex) == TOK_BRACE_CLOSE ) {
                throw ParseError::Unexpected(lex, tok);
            }
            // Keep looping until a comma
            do {
                if( LOOK_AHEAD(lex) == TOK_BRACE_CLOSE ) {
                    // Trailing comma
                    GET_TOK(tok, lex);
                    break;
                }
                // - Handle `self` in braces differently
                else if( LOOK_AHEAD(lex) == TOK_RWORD_SELF ) {
                    GET_TOK(tok, lex);
                    auto name = path.nodes().back().name();
                    if( LOOK_AHEAD(lex) == TOK_RWORD_AS ) {
                        GET_TOK(tok, lex);
                        GET_CHECK_TOK(tok, lex, TOK_IDENT);
                        name = tok.istr();
                    }
                    entries.push_back({ lex.point_span(), AST::Path(path), ::std::move(name) });
                }
                else {
                    size_t l = path.nodes().size();

                    Parse_Use_Inner(lex, entries, path);

                    assert(l <= path.nodes().size());
                    path.nodes().resize( l );
                }
            } while( GET_TOK(tok, lex) == TOK_COMMA );
            CHECK_TOK(tok, TOK_BRACE_CLOSE);
            return;
        case TOK_STAR:
            entries.push_back({ lex.point_span(), AST::Path(path), "" });
            return ;
        default:
            throw ParseError::Unexpected(lex, tok);
        }
    } while( GET_TOK(tok, lex) == TOK_DOUBLE_COLON );

    RcString name;

    // NOTE: The above loop has to run once, so the last token HAS to have been an ident
    if( tok.type() == TOK_RWORD_AS )
    {
        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        name = tok.istr();
    }
    else
    {
        PUTBACK(tok, lex);
        ASSERT_BUG(lex.point_span(), path.nodes().size() > 0, "`use` with no path");
        name = path.nodes().back().name();
    }

    // TODO: Get a span covering the final node.
    entries.push_back({ lex.point_span(), AST::Path(path), ::std::move(name) });
}
::AST::UseItem Parse_Use(TokenStream& lex)
{
    TRACE_FUNCTION;

    Token   tok;
    AST::Path   path = AST::Path("", {});
    ::std::vector<AST::PathNode>    nodes;
    ProtoSpan   span_start = lex.start_span();

    ::std::vector<AST::UseItem::Ent>    entries;

    switch( GET_TOK(tok, lex) )
    {
    case TOK_RWORD_SELF:
        path = AST::Path( AST::Path::TagSelf(), {} );    // relative path
        GET_CHECK_TOK(tok, lex, TOK_DOUBLE_COLON);
        break;
    case TOK_RWORD_SUPER: {
        unsigned int count = 1;
        while( LOOK_AHEAD(lex) == TOK_DOUBLE_COLON && lex.lookahead(1) == TOK_RWORD_SUPER ) {
            GET_CHECK_TOK(tok, lex, TOK_DOUBLE_COLON);
            GET_CHECK_TOK(tok, lex, TOK_RWORD_SUPER);
            count += 1;
        }
        path = AST::Path( AST::Path::TagSuper(), count, {} );
        GET_CHECK_TOK(tok, lex, TOK_DOUBLE_COLON);
        break; }
    case TOK_RWORD_CRATE:
        // 1.29 absolute path
        GET_CHECK_TOK(tok, lex, TOK_DOUBLE_COLON);
        break;
    // Leading :: is allowed and ignored for the $crate feature
    case TOK_DOUBLE_COLON:
        // Absolute path
        // HACK! mrustc emits $crate as `::"crate-name"`
        if( LOOK_AHEAD(lex) == TOK_STRING )
        {
            GET_CHECK_TOK(tok, lex, TOK_STRING);
            path = ::AST::Path(RcString::new_interned(tok.str()), {});
        }
        else
        {
            PUTBACK(tok, lex);
        }
        GET_CHECK_TOK(tok, lex, TOK_DOUBLE_COLON);
        break;
    case TOK_INTERPOLATED_PATH:
        path = mv$(tok.frag_path());
        GET_CHECK_TOK(tok, lex, TOK_DOUBLE_COLON);
        break;
    default:
        PUTBACK(tok, lex);
        break;
    }

    Parse_Use_Inner(lex, entries, path);

    return AST::UseItem { lex.end_span(span_start), mv$(entries) };
}


::AST::MacroInvocation Parse_MacroInvocation(ProtoSpan span_start, RcString name, TokenStream& lex)
{
    Token   tok;
    RcString ident;
    if( GET_TOK(tok, lex) == TOK_IDENT ) {
        ident = tok.istr();
    }
    else {
        PUTBACK(tok, lex);
    }
    DEBUG("name=" << name << ", ident=" << ident);
    TokenTree tt = Parse_TT(lex, true);
    return ::AST::MacroInvocation( lex.end_span(span_start), mv$(name), mv$(ident), mv$(tt));
}

bool Parse_MacroInvocation_Opt(TokenStream& lex,  AST::MacroInvocation& out_inv)
{
    Token   tok;
    if( lex.lookahead(0) == TOK_IDENT && lex.lookahead(1) == TOK_EXCLAM )
    {
        // Good
    }
    else if( lex.lookahead(0) == TOK_INTERPOLATED_PATH && lex.lookahead(1) == TOK_EXCLAM )
    {
        // Also good.
    }
    else
    {
        return false;
    }

    auto ps = lex.start_span();
    auto name_path = Parse_Path(lex, PATH_GENERIC_NONE);
    ASSERT_BUG(lex.point_span(), name_path.nodes().size() == 1, "TODO: Support multi-component paths in macro invocations");
    auto name = name_path.nodes()[0].name();
    GET_CHECK_TOK(tok, lex, TOK_EXCLAM);

    bool is_braced = (lex.lookahead(0) == TOK_BRACE_OPEN || (lex.lookahead(0) == TOK_IDENT && lex.lookahead(1) == TOK_BRACE_OPEN));

    out_inv = Parse_MacroInvocation(ps, name, lex);

    if(!is_braced )
    {
        GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
    }
    return true;
}

#if 0
namespace {
    ::AST::Module::FileInfo get_submod_file(
        Span sp, const ::AST::Module::FileInfo& parent_fileinfo, const ::std::string& new_mod_name, const ::std::string& path_attr,
        bool is_extern_file, bool is_cfg_disabled
        )
    {
        ::AST::Module::FileInfo rv;
        TRACE_FUNCTION_F(new_mod_name << ", " << path_attr << ", " << is_extern_file);
        ::std::string   sub_path;
        bool    sub_file_controls_dir = true;

        // 1. Determine the base path for the module
        if( parent_fileinfo.path == "-" ) {
            if( path_attr.size() ) {
                ERROR(lex.point_span(), E0000, "Cannot load module from file when reading stdin");
            }
            sub_path = "-";
        }
        else if( path_attr.size() > 0 )
        {
            // #[path] present, add to the parent path
            sub_path = dirname(parent_fileinfo.path) + path_attr;
        }
        else if( mod_fileinfo.controls_dir )
        {
            // The parent module is either the crate root, or a `mod.rs` (or otherwise a controller)
            sub_path = parent_fileinfo.dir + name;
        }
        else
        {
            sub_path = parent_fileinfo.path;
            sub_file_controls_dir = false;
        }
        DEBUG("Mod '" << name << "', sub_path = " << sub_path);

        rv.path = sub_path;
        rv.controls_dir = sub_file_controls_dir;

        if( is_cfg_disabled || parent_fileinfo.force_no_load )
        {
            rv.force_no_load = true;
        }
        if( ! is_extern_file )
        {
            // If this is an inline module, set the path to just a directory
            if( sub_path != "-" ) {
                rv.path = sub_path + "/";
                rv.dir = sub_path + "/";
            }
            else {
                rv.path = "-";
            }
        }
        else
        {
            if( sub_path == "-" )
            {
                ERROR(lex.point_span(), E0000, "Cannot load module from file when reading stdin");
            }
            else
            {
                ::std::string newpath_dir  = sub_path + "/";
                ::std::string newpath_file = path_attr.size() > 0 ? sub_path : sub_path + ".rs";
                DEBUG("newpath_dir = '" << newpath_dir << "', newpath_file = '" << newpath_file << "'");
                ::std::ifstream ifs_dir (newpath_dir + "mod.rs");
                ::std::ifstream ifs_file(newpath_file);
                if( ifs_dir.is_open() && ifs_file.is_open() )
                {
                    // Collision
                    ERROR(lex.point_span(), E0000, "Both modname.rs and modname/mod.rs exist");
                }
                else if( ifs_dir.is_open() )
                {
                    // Load from dir
                    rv.path = newpath_dir + "mod.rs";
                    rv.dir = newpath_dir;
                }
                else if( ifs_file.is_open() )
                {
                    rv.path = newpath_file;
                    rv.dir = newpath_dir;
                    rv.controls_dir = false;
                }
                else
                {
                    // Can't find file
                    ERROR(sp, E0000, "Can't find file for '" << name << "' in '" << parent_fileinfo.dir << "'");
                }
            }
        }
        return rv;
    }
}
#endif

::AST::Named<::AST::Item> Parse_Mod_Item_S(TokenStream& lex, const AST::Module::FileInfo& mod_fileinfo, const ::AST::Path& mod_path, AST::AttributeList meta_items)
{
    TRACE_FUNCTION_F("mod_path="<<mod_path<<", meta_items="<<meta_items);
    Token   tok;

    // NOTE: This assigns into a parameter, so can't use Parse_ItemAttrs
    while( LOOKAHEAD2(lex, TOK_HASH, TOK_SQUARE_OPEN) )
    {
        // Attributes!
        GET_CHECK_TOK(tok, lex, TOK_HASH);
        GET_CHECK_TOK(tok, lex, TOK_SQUARE_OPEN);
        meta_items.push_back( Parse_MetaItem(lex) );
        GET_CHECK_TOK(tok, lex, TOK_SQUARE_CLOSE);
    }

    if( LOOK_AHEAD(lex) == TOK_INTERPOLATED_ITEM ) {
        GET_TOK(tok, lex);
        auto rv = tok.take_frag_item();
        // Transfer new attributes onto the item
        for(auto& mi : meta_items.m_items)
            rv.attrs.m_items.push_back( mv$(mi) );
        return rv;
    }

    auto ps = lex.start_span();

    RcString item_name;
    ::AST::Item item_data;

    {
        ::AST::MacroInvocation  inv;
        if( Parse_MacroInvocation_Opt(lex, inv) )
        {
            return ::AST::Named< ::AST::Item> { lex.end_span(ps), mv$(meta_items), false, "", ::AST::Item( mv$(inv) ) };
        }
    }

    bool    is_public = Parse_Publicity(lex);

    switch( GET_TOK(tok, lex) )
    {
    case TOK_RWORD_USE:
        item_data = Parse_Use(lex);
        GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
        break;

    case TOK_RWORD_EXTERN:
        switch( GET_TOK(tok, lex) )
        {
        // `extern "<ABI>" fn ...`
        // `extern "<ABI>" { ...`
        case TOK_STRING: {
            ::std::string abi = tok.str();
            switch(GET_TOK(tok, lex))
            {
            // `extern "<ABI>" fn ...`
            case TOK_RWORD_FN: {
                GET_CHECK_TOK(tok, lex, TOK_IDENT);
                item_name = tok.istr();
                item_data = ::AST::Item( Parse_FunctionDefWithCode(lex, abi, false,  false,false) );
                break; }
            // `extern "ABI" {`
            case TOK_BRACE_OPEN:
                item_name = "";
                item_data = ::AST::Item( Parse_ExternBlock(lex, mv$(abi), meta_items) );
                break;
            default:
                throw ParseError::Unexpected(lex, tok, {TOK_RWORD_FN, TOK_BRACE_OPEN});
            }
            break; }
        // `extern fn ...`
        case TOK_RWORD_FN:
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            item_name = tok.istr();
            item_data = ::AST::Item( Parse_FunctionDefWithCode(lex, "C", false,  false,false) );
            break;

        // NOTE: `extern { ...` is handled in caller
        case TOK_BRACE_OPEN:
            item_name = "";
            item_data = ::AST::Item( Parse_ExternBlock(lex, "C", meta_items) );
            break;

        // `extern crate "crate-name" as crate_name;`
        // `extern crate crate_name;`
        // `extern crate crate_name as other_name;`
        case TOK_RWORD_CRATE:
            switch( GET_TOK(tok, lex) )
            {
            // `extern crate "crate-name" as crate_name;`
            // NOTE: rustc doesn't allow this, keep in mrustc for for reparse support
            case TOK_STRING:
                item_data = ::AST::Item::make_Crate({ RcString::new_interned(tok.str()) });
                GET_CHECK_TOK(tok, lex, TOK_RWORD_AS);
                GET_CHECK_TOK(tok, lex, TOK_IDENT);
                item_name = tok.istr();
                break;
            // `extern crate crate_name;`
            // `extern crate crate_name as other_name;`
            case TOK_IDENT:
                item_name = tok.istr();
                if(GET_TOK(tok, lex) == TOK_RWORD_AS) {
                    item_data = ::AST::Item::make_Crate({ mv$(item_name) });

                    GET_CHECK_TOK(tok, lex, TOK_IDENT);
                    item_name = tok.istr();
                }
                else {
                    PUTBACK(tok, lex);
                    item_data = ::AST::Item::make_Crate({ item_name });
                }
                break;
            default:
                throw ParseError::Unexpected(lex, tok, {TOK_STRING, TOK_IDENT});
            }
            GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
            break;
        default:
            throw ParseError::Unexpected(lex, tok, {TOK_STRING, TOK_RWORD_FN, TOK_BRACE_OPEN, TOK_RWORD_CRATE});
        }
        break;

    // `const NAME`
    // `const [unsafe] fn`
    case TOK_RWORD_CONST:
        switch( GET_TOK(tok, lex) )
        {
        case TOK_IDENT: {
            item_name = tok.istr();

            GET_CHECK_TOK(tok, lex, TOK_COLON);
            TypeRef type = Parse_Type(lex);
            GET_CHECK_TOK(tok, lex, TOK_EQUAL);
            AST::Expr val = Parse_Expr(lex);
            GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
            item_data = ::AST::Item( ::AST::Static(AST::Static::CONST, mv$(type), mv$(val)) );
            break; }
        case TOK_RWORD_UNSAFE:
            GET_CHECK_TOK(tok, lex, TOK_RWORD_FN);
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            item_name = tok.istr();
            item_data = ::AST::Item( Parse_FunctionDefWithCode(lex, ABI_RUST, false,  true,true/*unsafe,const*/) );
            break;
        case TOK_RWORD_FN:
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            item_name = tok.istr();
            // - self not allowed, not prototype
            item_data = ::AST::Item( Parse_FunctionDefWithCode(lex, ABI_RUST, false,  false,true/*unsafe,const*/) );
            break;
        default:
            throw ParseError::Unexpected(lex, tok, {TOK_IDENT, TOK_RWORD_FN});
        }
        break;
    // `static NAME`
    // `static mut NAME`
    case TOK_RWORD_STATIC: {
        bool is_mut = false;
        if(GET_TOK(tok, lex) == TOK_RWORD_MUT) {
            is_mut = true;
            GET_TOK(tok, lex);
        }
        CHECK_TOK(tok, TOK_IDENT);
        item_name = tok.istr();

        GET_CHECK_TOK(tok, lex, TOK_COLON);
        TypeRef type = Parse_Type(lex);

        GET_CHECK_TOK(tok, lex, TOK_EQUAL);

        AST::Expr val = Parse_Expr(lex);

        GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
        item_data = ::AST::Item( ::AST::Static( (is_mut ? AST::Static::MUT : AST::Static::STATIC), mv$(type), mv$(val)) );
        break; }

    // `unsafe fn`
    // `unsafe trait`
    // `unsafe impl`
    case TOK_RWORD_UNSAFE:
        switch(GET_TOK(tok, lex))
        {
        // `unsafe extern fn`
        case TOK_RWORD_EXTERN: {
            ::std::string   abi = "C";
            if(GET_TOK(tok, lex) == TOK_STRING) {
                abi = mv$(tok.str());
            }
            else {
                PUTBACK(tok, lex);
            }
            GET_CHECK_TOK(tok, lex, TOK_RWORD_FN);
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            item_name = tok.istr();
            item_data = ::AST::Item( Parse_FunctionDefWithCode(lex, abi, false,  true,false/*unsafe,const*/) );
            break; }
        // `unsafe fn`
        case TOK_RWORD_FN:
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            item_name = tok.istr();
            // - self not allowed, not prototype
            item_data = ::AST::Item( Parse_FunctionDefWithCode(lex, ABI_RUST, false,  true,false/*unsafe,const*/) );
            break;
        // `unsafe trait`
        case TOK_RWORD_TRAIT: {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            item_name = tok.istr();
            auto tr = Parse_TraitDef(lex, meta_items);
            tr.set_is_unsafe();
            item_data = ::AST::Item( ::std::move(tr) );
            break; }
        // `unsafe impl`
        case TOK_RWORD_IMPL: {
            auto impl = Parse_Impl(lex, mv$(meta_items), true);
            if( impl.is_Impl() ) {
                impl.as_Impl().def().set_is_unsafe();
            }
            else if( impl.is_NegImpl() ) {
                impl.as_NegImpl().set_is_unsafe();
            }
            else {
                BUG(lex.point_span(), "Parse_Impl returned a variant other than Impl or NegImpl");
            }
            return ::AST::Named< ::AST::Item> { Span(), {}, false, "", mv$(impl) };
            }
        // `unsafe auto trait`
        case TOK_IDENT:
            if( TARGETVER_1_29 && tok.istr() == "auto" ) {
                GET_CHECK_TOK(tok, lex, TOK_RWORD_TRAIT);
                GET_CHECK_TOK(tok, lex, TOK_IDENT);
                item_name = tok.istr();
                auto tr = Parse_TraitDef(lex, meta_items);
                tr.set_is_unsafe();
                tr.set_is_marker();
                item_data = ::AST::Item( ::std::move(tr) );
                break;
            }
            //goto default;
        default:
            throw ParseError::Unexpected(lex, tok, {TOK_RWORD_FN, TOK_RWORD_TRAIT, TOK_RWORD_IMPL});
        }
        break;
    // `fn`
    case TOK_RWORD_FN:
        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        item_name = tok.istr();
        // - self not allowed, not prototype
        item_data = ::AST::Item( Parse_FunctionDefWithCode(lex, ABI_RUST, false,  false,false/*unsafe,const*/) );
        break;
    // `type`
    case TOK_RWORD_TYPE:
        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        item_name = tok.istr();
        item_data = ::AST::Item( Parse_TypeAlias(lex) );
        break;
    // `struct`
    case TOK_RWORD_STRUCT:
        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        item_name = tok.istr();
        item_data = ::AST::Item( Parse_Struct(lex, meta_items) );
        break;
    // `enum`
    case TOK_RWORD_ENUM:
        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        item_name = tok.istr();
        item_data = ::AST::Item( Parse_EnumDef(lex, meta_items) );
        break;
    // Contextual keywords
    case TOK_IDENT:
        if( tok.istr() == "union" ) {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            item_name = tok.istr();
            item_data = ::AST::Item( Parse_Union(lex, meta_items) );
        }
        // `auto trait`
        else if( TARGETVER_1_29 && tok.istr() == "auto" ) {
            GET_CHECK_TOK(tok, lex, TOK_RWORD_TRAIT);
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            item_name = tok.istr();
            auto tr = Parse_TraitDef(lex, meta_items);
            tr.set_is_marker();
            item_data = ::AST::Item( ::std::move(tr) );
        }
        else {
            throw ParseError::Unexpected(lex, tok);
        }
        break;
    // `impl`
    case TOK_RWORD_IMPL:
        return ::AST::Named< ::AST::Item> { Span(), {}, false, "", Parse_Impl(lex, mv$(meta_items)) };
    // `trait`
    case TOK_RWORD_TRAIT:
        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        item_name = tok.istr();
        item_data = ::AST::Item( Parse_TraitDef(lex, meta_items) );
        break;

    case TOK_RWORD_MACRO:
        if( TARGETVER_1_29 )
        {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            auto name = tok.istr();
            if( lex.lookahead(0) != TOK_PAREN_OPEN )
            {
                GET_TOK(tok, lex);
                throw ParseError::Unexpected(lex, tok);
            }
            DEBUG("name = " << name);

            ::std::vector<RcString>   names;
            auto ps = lex.start_span();
            GET_CHECK_TOK(tok, lex, TOK_PAREN_OPEN);
            auto arm_pat = Parse_MacroRules_Pat(lex, TOK_PAREN_OPEN, TOK_PAREN_CLOSE, names);
            auto pat_span = lex.end_span(ps);
            GET_CHECK_TOK(tok, lex, TOK_BRACE_OPEN);
            // TODO: Pass a flag that annotates all idents with the current module?
            auto body = Parse_MacroRules_Cont(lex, TOK_BRACE_OPEN, TOK_BRACE_CLOSE, names);

            auto mr = new MacroRules( );
            mr->m_hygiene = lex.getHygiene();
            {
                Ident::ModPath  mp;
                for(const auto& node : mod_path.nodes())
                {
                    mp.ents.push_back(node.name());
                }
                mr->m_hygiene.set_mod_path(::std::move(mp));
            }
            mr->m_rules.push_back(Parse_MacroRules_MakeArm(pat_span, ::std::move(arm_pat), ::std::move(body)));

            item_name = name;
            item_data = ::AST::Item( MacroRulesPtr(mr) );
        }
        else
        {
            throw ParseError::Unexpected(lex, tok);
        }
        break;

    case TOK_RWORD_MOD: {
        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        auto name = tok.istr();
        DEBUG("Sub module '" << name << "'");
        AST::Module submod( mod_path + name );

        // Check #[cfg] and don't load if it fails
        struct H {
            static bool check_item_cfg(const ::AST::AttributeList& attrs)
            {
                for(const auto& at : attrs.m_items) {
                    if( at.name() == "cfg" && !check_cfg(at.span(), at) ) {
                        return false;
                    }
                }
                return true;
            }
        };

        // Rules for external files (/ path handling):
        // - IF using stdin (path='-') - Disallow and propagate '-' as path
        // - IF a #[path] attribute was passed, allow
        // - IF in crate root or mod.rs, allow (input flag)
        // - else, disallow and set flag
        ::std::string path_attr;
        for(const auto& a : meta_items.m_items)
        {
            if( a.name() == "path" ) {
                path_attr = a.string();
            }
            else if( a.name() == "cfg_attr" && a.items().at(1).name() == "path" ) {
                if( check_cfg(a.span(), a.items().at(0)) ) {
                    path_attr = a.items().at(1).string();
                }
            }
            else {
            }
        }

        //submod.m_file_info = get_submod_file(lex.end_span(ps), mod_fileinfo,  name, path_attr, LOOK_AHEAD(lex) == TOK_SEMICOLON, H::check_item_cfg(meta_items));

        ::helpers::path sub_path;
        bool    sub_file_controls_dir = true;
        if( mod_fileinfo.path == "-" ) {
            if( path_attr.size() ) {
                ERROR(lex.point_span(), E0000, "Cannot load module from file when reading stdin");
            }
            sub_path = "-";
        }
        else if( path_attr.size() > 0 )
        {
            sub_path = dirname(mod_fileinfo.path) / path_attr.c_str();
        }
        else if( mod_fileinfo.controls_dir )
        {
            sub_path = dirname(mod_fileinfo.path) / name.c_str();
        }
        else
        {
            sub_path = dirname(mod_fileinfo.path) / mod_path.nodes().back().name().c_str() / name.c_str();
            //sub_path = mod_fileinfo.path;
            sub_file_controls_dir = false;
        }
        DEBUG("Mod '" << name << "', sub_path = " << sub_path);

        submod.m_file_info.path = sub_path;
        submod.m_file_info.controls_dir = sub_file_controls_dir;

        switch( GET_TOK(tok, lex) )
        {
        case TOK_BRACE_OPEN:
            submod.m_file_info.path = sub_path.str() + "/";
            // TODO: If cfg fails, just eat the TT until a matching #[cfg]?
            // - Or, mark the file infor as not being valid (so child modules don't try to load)
            Parse_ModRoot(lex, submod, meta_items);
            GET_CHECK_TOK(tok, lex, TOK_BRACE_CLOSE);
            break;
        case TOK_SEMICOLON:
            if( sub_path.str() == "-" ) {
                ERROR(lex.point_span(), E0000, "Cannot load module from file when reading stdin");
            }
            else if( !H::check_item_cfg(meta_items) ) {
                // Ignore - emit Item::None
                item_name = mv$(name);
                item_data = ::AST::Item( );
                break ;
            }
            else if( path_attr.size() == 0 && ! mod_fileinfo.controls_dir )
            {
                // TODO: Also search for curmod/submod.rs
                //::std::string newpath_file = (mod_path.nodes().size() > 1 ? dirname(mod_fileinfo.path) + mod_path.nodes()[mod_path.nodes().size()-2].name() + "/" + name + ".rs" : "");
                ::std::string newpath_file = (mod_path.nodes().size() >= 1 ? dirname(mod_fileinfo.path) / mod_path.nodes()[mod_path.nodes().size()-1].name().c_str() / name.c_str() + ".rs" : "");
                DEBUG("newpath_file = '" << newpath_file << "' " << mod_fileinfo.path << " " << mod_path);
                ::std::ifstream ifs_file(newpath_file);
                if( ifs_file.is_open() )
                {
                    submod.m_file_info.path = newpath_file;
                    submod.m_file_info.controls_dir = false;
                    DEBUG("- path = " << submod.m_file_info.path);
                    Lexer sub_lex(submod.m_file_info.path);
                    Parse_ModRoot(sub_lex, submod, meta_items);
                    GET_CHECK_TOK(tok, sub_lex, TOK_EOF);
                }
                else
                {
                    ERROR(lex.point_span(), E0000, "Can't load from files outside of mod.rs or crate root");
                }
            }
            else
            {
                ::std::string newpath_dir  = sub_path.str() + "/";
                ::std::string newpath_file = path_attr.size() > 0 ? sub_path : sub_path + ".rs";
                DEBUG("newpath_dir = '" << newpath_dir << "', newpath_file = '" << newpath_file << "'");
                ::std::ifstream ifs_dir (newpath_dir + "mod.rs");
                ::std::ifstream ifs_file(newpath_file);
                if( ifs_dir.is_open() && ifs_file.is_open() )
                {
                    // Collision
                    ERROR(lex.point_span(), E0000, "Both modname.rs and modname/mod.rs exist");
                }
                else if( ifs_dir.is_open() )
                {
                    // Load from dir
                    submod.m_file_info.path = newpath_dir + "mod.rs";
                }
                else if( ifs_file.is_open() )
                {
                    submod.m_file_info.path = newpath_file;
                    if( path_attr == "" )
                    {
                        submod.m_file_info.controls_dir = false;
                    }
                }
                // TODO: If this is not a controlling file, look in `modname/` for the new module
                else
                {
                    // Can't find file
                    ERROR(lex.point_span(), E0000, "Can't find file for '" << name << "' in '" << mod_fileinfo.path << "'");
                }
                DEBUG("- path = " << submod.m_file_info.path);
                Lexer sub_lex(submod.m_file_info.path);
                Parse_ModRoot(sub_lex, submod, meta_items);
                GET_CHECK_TOK(tok, sub_lex, TOK_EOF);
            }
            break;
        default:
            throw ParseError::Generic("Expected { or ; after module name");
        }
        item_name = mv$(name);
        item_data = ::AST::Item( mv$(submod) );
        break; }

    default:
        throw ParseError::Unexpected(lex, tok);
    }

    return ::AST::Named< ::AST::Item> { lex.end_span(ps), mv$(meta_items), is_public, mv$(item_name), mv$(item_data) };
}

void Parse_Mod_Item(TokenStream& lex, AST::Module& mod, AST::AttributeList meta_items)
{
    SET_MODULE(lex, mod);
    lex.parse_state().parent_attrs = &meta_items;

    mod.add_item( Parse_Mod_Item_S(lex, mod.m_file_info, mod.path(), mv$(meta_items)) );
}

void Parse_ModRoot_Items(TokenStream& lex, AST::Module& mod)
{
    Token   tok;

    for(;;)
    {
        // Check 1 - End of module (either via a closing brace, or EOF)
        switch(GET_TOK(tok, lex))
        {
        case TOK_BRACE_CLOSE:
        case TOK_EOF:
            PUTBACK(tok, lex);
            return;
        default:
            PUTBACK(tok, lex);
            break;
        }

        // Attributes on the following item
        auto meta_items = Parse_ItemAttrs(lex);
        DEBUG("meta_items = " << meta_items);

        Parse_Mod_Item(lex, mod, mv$(meta_items));
    }
}

void Parse_ModRoot(TokenStream& lex, AST::Module& mod, AST::AttributeList& mod_attrs)
{
    TRACE_FUNCTION;

    // Attributes on module/crate (will continue loop)
    Parse_ParentAttrs(lex,  mod_attrs);

    Parse_ModRoot_Items(lex, mod);
}

AST::Crate Parse_Crate(::std::string mainfile)
{
    Token   tok;

    Lexer lex(mainfile);

    size_t p = mainfile.find_last_of('/');
    p = (p == ::std::string::npos ? mainfile.find_last_of('\\') : p);
    ::std::string mainpath = (p != ::std::string::npos ? ::std::string(mainfile.begin(), mainfile.begin()+p+1) : "./");

    AST::Crate  crate;

    //crate.root_module().m_file_info.file_path = mainfile;
    crate.root_module().m_file_info.path = mainpath;
    crate.root_module().m_file_info.controls_dir = true;

    lex.parse_state().crate = &crate;
    Parse_ModRoot(lex, crate.root_module(), crate.m_attrs);

    return crate;
}
