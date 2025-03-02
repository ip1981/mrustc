/*
 */
#include "test_desc.h"
#include <parse/lex.hpp>
#include <parse/common.hpp>
#include <parse/parseerror.hpp>

#include <hir/hir.hpp>
#include <mir/mir.hpp>

namespace {
    HIR::Function parse_function(TokenStream& lex, RcString& out_name);
    HIR::PathParams parse_params(TokenStream& lex);
    HIR::Path parse_path(TokenStream& lex);
    HIR::TypeRef parse_type(TokenStream& lex);
    MIR::LValue parse_lvalue(TokenStream& lex, const ::std::map<RcString, MIR::LValue::Storage>& name_map);
    MIR::LValue parse_param(TokenStream& lex, const ::std::map<RcString, MIR::LValue::Storage>& name_map);

    bool consume_if(TokenStream& lex, eTokenType tok) {
        if( lex.lookahead(0) == tok ) {
            lex.getToken(); // eat token
            return true;
        }
        else {
            return false;
        }
    }
}

MirOptTestFile  MirOptTestFile::load_from_file(const helpers::path& p)
{
    Lexer   lex(p.str());
    Token   tok;

    MirOptTestFile  rv;
    rv.m_filename = p.basename();
    rv.m_crate = HIR::CratePtr(::HIR::Crate());
    //rv.m_crate->m_crate_name = RcString(p.str());

    while(lex.lookahead(0) != TOK_EOF)
    {
        std::map<RcString, std::string>  attrs;
        while( consume_if(lex, TOK_HASH) )
        {
            //bool is_outer = consume_if(lex, TOK_EXCLAM);
            GET_CHECK_TOK(tok, lex, TOK_SQUARE_OPEN);
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            auto name = tok.istr();
            GET_CHECK_TOK(tok, lex, TOK_EQUAL);
            GET_CHECK_TOK(tok, lex, TOK_STRING);
            auto value = tok.str();
            GET_CHECK_TOK(tok, lex, TOK_SQUARE_CLOSE);

            attrs.insert( std::make_pair(name, value) );
        }

        if( consume_if(lex, TOK_RWORD_FN) )
        {
            RcString    fcn_name;
            auto fcn_decl = parse_function(lex, fcn_name);

            auto vi = ::HIR::VisEnt<HIR::ValueItem> {
                HIR::Publicity::new_global(), ::HIR::ValueItem(mv$(fcn_decl))
                };
            rv.m_crate->m_root_module.m_value_items.insert(::std::make_pair(fcn_name,
                ::std::make_unique<decltype(vi)>(mv$(vi))
                ));

            // Attributes
            for(const auto& attr : attrs)
            {
                if( attr.first == "test" )
                {
                    MirOptTestFile::Test    t;
                    t.input_function = ::HIR::SimplePath("", { fcn_name });
                    t.output_template_function = ::HIR::SimplePath("", { RcString(attr.second) });

                    rv.m_tests.push_back(mv$(t));
                }
                else
                {
                    // Ignore?
                }
            }
        }
        //else if( lex.lookahead(0) == "INCLUDE" )
        //{
        //    auto path = lex.check_consume(TokenClass::String).strval;
        //}
        else
        {
            TODO(lex.point_span(), "Error");
        }
    }

    return rv;
}
namespace {
    HIR::Function parse_function(TokenStream& lex, RcString& out_name)
    {
        Token   tok;
        ::std::map<RcString, MIR::LValue::Storage>  val_name_map;
        val_name_map.insert(::std::make_pair("retval", MIR::LValue::Storage::new_Return()));
        ::std::map<RcString, unsigned>  dropflag_names;
        ::std::map<RcString, unsigned>  real_bb_name_map;
        ::std::map<RcString, unsigned>  lookup_bb_name_map;
        ::std::vector<RcString> lookup_bb_names;

        ::HIR::Function fcn_decl;
        fcn_decl.m_code.m_mir = ::MIR::FunctionPointer(new ::MIR::Function());
        auto& mir_fcn = *fcn_decl.m_code.m_mir;

        // Name
        GET_CHECK_TOK(tok, lex, TOK_IDENT);
        auto fcn_name = tok.istr();
        DEBUG("fn " << fcn_name);

        if(consume_if(lex, TOK_LT) )
        {
            TODO(lex.point_span(), "Generic functions");
        }

        // Arguments
        auto& args = fcn_decl.m_args;
        GET_CHECK_TOK(tok, lex, TOK_PAREN_OPEN);
        while( lex.lookahead(0) != TOK_PAREN_CLOSE )
        {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            auto name = tok.istr();
            GET_CHECK_TOK(tok, lex, TOK_COLON);
            auto var_ty = parse_type(lex);

            auto arg_idx = static_cast<unsigned>(args.size());
            val_name_map.insert( ::std::make_pair(name, ::MIR::LValue::Storage::new_Argument(arg_idx)) );
            args.push_back( ::std::make_pair(HIR::Pattern(), mv$(var_ty)) );

            if( !consume_if(lex, TOK_COMMA) )
                break;
        }
        GET_CHECK_TOK(tok, lex, TOK_PAREN_CLOSE);
        // Return type
        if( consume_if(lex, TOK_THINARROW) )
        {
            // Parse return type
            fcn_decl.m_return = parse_type(lex);
        }
        else
        {
            fcn_decl.m_return = HIR::TypeRef::new_unit();
        }

        // Body.
        GET_CHECK_TOK(tok, lex, TOK_BRACE_OPEN);

        // 1. Variable list
        while( consume_if(lex, TOK_RWORD_LET) )
        {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            auto name = tok.istr();
            if( consume_if(lex, TOK_EQUAL) ) {
                GET_TOK(tok, lex);
                bool v = tok.type() == TOK_RWORD_TRUE ? true
                    : tok.type() == TOK_RWORD_FALSE ? false
                    : throw ParseError::Unexpected(lex, tok, { TOK_RWORD_TRUE, TOK_RWORD_FALSE });
                GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);

                dropflag_names.insert(::std::make_pair( name, static_cast<unsigned>(mir_fcn.drop_flags.size()) ));
                mir_fcn.drop_flags.push_back(v);
            }
            else {
                GET_CHECK_TOK(tok, lex, TOK_COLON);
                auto var_ty = parse_type(lex);
                GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);

                auto var_idx = static_cast<unsigned>(mir_fcn.locals.size());
                val_name_map.insert( ::std::make_pair(name, ::MIR::LValue::Storage::new_Local(var_idx)) );
                mir_fcn.locals.push_back( mv$(var_ty) );
            }
        }
        // 2. List of BBs arranged with 'ident: { STMTS; TERM }'
        while( lex.lookahead(0) != TOK_BRACE_CLOSE )
        {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            auto name = tok.istr();
            real_bb_name_map.insert( ::std::make_pair(name, static_cast<unsigned>(mir_fcn.blocks.size())) );
            mir_fcn.blocks.push_back(::MIR::BasicBlock());
            auto& bb = mir_fcn.blocks.back();
            GET_CHECK_TOK(tok, lex, TOK_COLON);
            GET_CHECK_TOK(tok, lex, TOK_BRACE_OPEN);
            while( lex.lookahead(0) != TOK_BRACE_CLOSE )
            {
                GET_CHECK_TOK(tok, lex, TOK_IDENT);
                if( tok.istr() == "DROP" )
                {
                    auto slot = parse_lvalue(lex, val_name_map);
                    if( consume_if(lex, TOK_RWORD_IF) )
                    {
                        TODO(lex.point_span(), "MIR statement - DROP if");
                    }
                    else
                    {
                        bb.statements.push_back(::MIR::Statement::make_Drop({ MIR::eDropKind::DEEP, mv$(slot), ~0u }));
                    }
                }
                else if( tok.istr() == "ASM" )
                {
                    TODO(lex.point_span(), "MIR statement - " << tok);
                }
                else if( tok.istr() == "SETDROP" )
                {
                    TODO(lex.point_span(), "MIR statement - " << tok);
                }
                else if( tok.istr() == "ASSIGN" )
                {
                    // parse a lvalue
                    auto dst = parse_lvalue(lex, val_name_map);
                    GET_CHECK_TOK(tok, lex, TOK_EQUAL);

                    MIR::RValue src;
                    GET_TOK(tok, lex);
                    switch(tok.type())
                    {
                    // Constant boolean
                    case TOK_RWORD_TRUE:    src = MIR::Constant::make_Bool({true });   break;
                    case TOK_RWORD_FALSE:   src = MIR::Constant::make_Bool({false});   break;

                    case TOK_PLUS:
                    case TOK_DASH: {
                        bool is_neg = (tok.type() == TOK_DASH);
                        GET_TOK(tok, lex);
                        switch(tok.type())
                        {
                        case TOK_INTEGER:
                            TODO(lex.point_span(), "MIR assign - " << (is_neg ? "-" : "+") << tok.intval());
                            //src = MIR::Constant::make_Int({ tok.intval(), ct });
                        case TOK_FLOAT:
                            TODO(lex.point_span(), "MIR assign - " << (is_neg ? "-" : "+") << tok.floatval());
                            //src = MIR::Constant::make_Float({ tok.floatval(), ct });
                        default:
                            throw ParseError::Unexpected(lex, tok, { TOK_INTEGER, TOK_FLOAT });
                        }
                        } break;
                    case TOK_INTEGER:
                        TODO(lex.point_span(), "MIR assign - " << tok.intval());

                    case TOK_AMP:
                        if( consume_if(lex, TOK_RWORD_MOVE) )
                            src = MIR::RValue::make_Borrow({ 0, HIR::BorrowType::Owned, parse_lvalue(lex, val_name_map) });
                        else if( consume_if(lex, TOK_RWORD_MUT) )
                            src = MIR::RValue::make_Borrow({ 0, HIR::BorrowType::Unique, parse_lvalue(lex, val_name_map) });
                        else
                            src = MIR::RValue::make_Borrow({ 0, HIR::BorrowType::Shared, parse_lvalue(lex, val_name_map) });
                        break;

                    // Operator (e.g. `ADD(...)`) or an lvalue
                    case TOK_IDENT:
                        if( consume_if(lex, TOK_PAREN_OPEN) )
                        {
                            auto parse_binop = [&](TokenStream& lex, MIR::eBinOp op) {
                                Token   tok;
                                auto l = parse_param(lex, val_name_map);
                                GET_CHECK_TOK(tok, lex, TOK_COMMA);
                                auto r = parse_param(lex, val_name_map);
                                return MIR::RValue::make_BinOp({ mv$(l), op, mv$(r) });
                                };
                            if(tok.istr() == "ADD")
                                src = parse_binop(lex, MIR::eBinOp::ADD);
                            else if(tok.istr() == "SUB")
                                src = parse_binop(lex, MIR::eBinOp::SUB);
                            else
                            {
                                TODO(lex.point_span(), "MIR assign operator - " << tok.istr());
                            }
                            GET_CHECK_TOK(tok, lex, TOK_PAREN_CLOSE);
                        }
                        else
                        {
                            lex.putback(mv$(tok));
                            src = parse_lvalue(lex, val_name_map);
                        }
                        break;
                    // Start of a path (lvalue)
                    case TOK_LT:
                    case TOK_DOUBLE_LT:
                    case TOK_DOUBLE_COLON:
                        lex.putback(mv$(tok));
                        src = parse_lvalue(lex, val_name_map);
                        break;
                    // Tuple literal
                    case TOK_PAREN_OPEN: {
                        src = MIR::RValue::make_Tuple({});
                        auto& vals = src.as_Tuple().vals;
                        while( lex.lookahead(0) != TOK_PAREN_CLOSE )
                        {
                            vals.push_back(parse_param(lex, val_name_map));
                            if( !consume_if(lex, TOK_COMMA) )
                                break;
                        }
                        GET_CHECK_TOK(tok, lex, TOK_PAREN_CLOSE);
                        } break;
                    default:
                        TODO(lex.point_span(), "MIR assign - " << tok);
                    }

                    bb.statements.push_back(::MIR::Statement::make_Assign({ mv$(dst), mv$(src) }));
                }
                else
                {
                    TODO(lex.point_span(), "MIR statement - " << tok);
                }
                GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
            }
            GET_CHECK_TOK(tok, lex, TOK_BRACE_CLOSE);

            auto parse_bb_name = [&](TokenStream& lex) {
                Token   tok;
                GET_CHECK_TOK(tok, lex, TOK_IDENT);
                auto& bb_name = tok.istr();
                if( lookup_bb_name_map.count(bb_name) ) {
                    return lookup_bb_name_map[bb_name];
                }
                else {
                    unsigned idx = static_cast<unsigned>(lookup_bb_names.size());
                    lookup_bb_names.push_back(bb_name);
                    lookup_bb_name_map[bb_name] = idx;
                    return idx;
                }
                };

            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            if( tok.istr() == "RETURN" )
            {
                bb.terminator = ::MIR::Terminator::make_Return({});
            }
            else if( tok.istr() == "DIVERGE" )
            {
                bb.terminator = ::MIR::Terminator::make_Diverge({});
            }
            else if( tok.istr() == "GOTO" )
            {
                bb.terminator = ::MIR::Terminator::make_Goto(parse_bb_name(lex));
            }
            else if( tok.istr() == "PANIC" )
            {
                bb.terminator = ::MIR::Terminator::make_Panic({ parse_bb_name(lex) });
            }
            else if( tok.istr() == "CALL" )
            {
                auto dst = parse_lvalue(lex, val_name_map);
                GET_CHECK_TOK(tok, lex, TOK_EQUAL);

                // - Call target
                MIR::CallTarget target;
                if( lex.lookahead(0) == TOK_PAREN_OPEN )
                {
                    GET_TOK(tok, lex);
                    target = ::MIR::CallTarget::make_Value(parse_lvalue(lex, val_name_map));
                }
                else if( lex.lookahead(0) == TOK_STRING )
                {
                    GET_TOK(tok, lex);
                    auto int_name = RcString(tok.str());
                    auto params = parse_params(lex);
                    target = ::MIR::CallTarget::make_Intrinsic({ int_name, mv$(params) });
                }
                else
                {
                    target = ::MIR::CallTarget::make_Path( parse_path(lex) );
                }

                // - Arguments
                ::std::vector<MIR::Param>   args;
                GET_CHECK_TOK(tok, lex, TOK_PAREN_OPEN);
                while( lex.lookahead(0) != TOK_PAREN_CLOSE )
                {
                    args.push_back(parse_param(lex, val_name_map));
                    if( !consume_if(lex, TOK_COMMA) )
                        break;
                }
                GET_CHECK_TOK(tok, lex, TOK_PAREN_CLOSE);

                // - Target blocks
                GET_CHECK_TOK(tok, lex, TOK_FATARROW);
                auto ret_bb = parse_bb_name(lex);
                GET_CHECK_TOK(tok, lex, TOK_RWORD_ELSE);
                auto panic_bb = parse_bb_name(lex);

                bb.terminator = ::MIR::Terminator::make_Call({ ret_bb, panic_bb, mv$(dst), mv$(target), mv$(args) });
            }
            else if( tok.istr() == "IF" )
            {
                auto v = parse_lvalue(lex, val_name_map);
                GET_CHECK_TOK(tok, lex, TOK_FATARROW);
                auto bb0 = parse_bb_name(lex);
                GET_CHECK_TOK(tok, lex, TOK_RWORD_ELSE);
                auto bb1 = parse_bb_name(lex);

                bb.terminator = ::MIR::Terminator::make_If({ mv$(v), bb0, bb1 });
            }
            else if( tok.istr() == "SWITCH" )
            {
                auto v = parse_lvalue(lex, val_name_map);
                ::std::vector<unsigned> targets;

                if( lex.lookahead(0) == TOK_IDENT )
                {
                    GET_CHECK_TOK(tok, lex, TOK_IDENT);
                    if( tok.istr() == "str" )
                    {
                        TODO(lex.point_span(), "MIR terminator - SwitchValue - str");
                    }
                    else if( tok.istr() == "sint" )
                    {
                        TODO(lex.point_span(), "MIR terminator - SwitchValue - signed");
                    }
                    else if( tok.istr() == "uint" )
                    {
                        TODO(lex.point_span(), "MIR terminator - SwitchValue - unsigned");
                    }
                    else
                    {
                        TODO(lex.point_span(), "MIR terminator - SwitchValue - unknown " << tok);
                    }
                }
                else
                {
                    GET_CHECK_TOK(tok, lex, TOK_BRACE_OPEN);
                    while( lex.lookahead(0) != TOK_BRACE_CLOSE )
                    {
                        targets.push_back(parse_bb_name(lex));
                        if( !consume_if(lex, TOK_COMMA) )
                            break;
                    }
                    GET_CHECK_TOK(tok, lex, TOK_BRACE_CLOSE);

                    bb.terminator = ::MIR::Terminator::make_Switch({ mv$(v), targets });
                }
            }
            else
            {
                TODO(lex.point_span(), "MIR terminator - " << tok.istr());
            }
            GET_CHECK_TOK(tok, lex, TOK_SEMICOLON);
        }
        GET_CHECK_TOK(tok, lex, TOK_BRACE_CLOSE);

        // 3. Convert BB indexes into correct numbering
        for(auto& blk : mir_fcn.blocks)
        {
            TU_MATCH_HDRA( (blk.terminator), {)
            TU_ARMA(Diverge, e) {
                }
            TU_ARMA(Return, e) {
                }
            TU_ARMA(Incomplete, e) {
                }
            TU_ARMA(Goto, e) {
                e = real_bb_name_map.at( lookup_bb_names[e] );
                }
            TU_ARMA(Panic, e) {
                e.dst = real_bb_name_map.at( lookup_bb_names[e.dst] );
                }
            TU_ARMA(Call, e) {
                e.ret_block = real_bb_name_map.at( lookup_bb_names[e.ret_block] );
                e.panic_block = real_bb_name_map.at( lookup_bb_names[e.panic_block] );
                }
            TU_ARMA(If, e) {
                e.bb0 = real_bb_name_map.at( lookup_bb_names[e.bb0] );
                e.bb1 = real_bb_name_map.at( lookup_bb_names[e.bb1] );
                }
            TU_ARMA(Switch, e) {
                for(auto& tgt : e.targets)
                    tgt = real_bb_name_map.at( lookup_bb_names[tgt] );
                }
            TU_ARMA(SwitchValue, e) {
                for(auto& tgt : e.targets)
                    tgt = real_bb_name_map.at( lookup_bb_names[tgt] );
                }
            }
        }

        DEBUG(fcn_decl.m_args << " -> " << fcn_decl.m_return);
        out_name = mv$(fcn_name);
        return fcn_decl;
    }
    HIR::PathParams parse_params(TokenStream& lex)
    {
        HIR::PathParams rv;
        if( lex.lookahead(0) == TOK_LT || lex.lookahead(0) == TOK_DOUBLE_LT )
        {
            TODO(lex.point_span(), "parse_params");
        }
        return rv;
    }

    HIR::SimplePath parse_simplepath(TokenStream& lex)
    {
        Token   tok;
        GET_CHECK_TOK(tok, lex, TOK_DOUBLE_COLON);
        GET_CHECK_TOK(tok, lex, TOK_STRING);
        auto rv = ::HIR::SimplePath(RcString::new_interned(tok.str()));

        GET_CHECK_TOK(tok, lex, TOK_DOUBLE_COLON);
        lex.putback(mv$(tok));

        while( consume_if(lex, TOK_DOUBLE_COLON) )
        {
            GET_CHECK_TOK(tok, lex, TOK_IDENT);
            rv.m_components.push_back( tok.istr() );
        }
        return rv;
    }
    HIR::GenericPath parse_genericpath(TokenStream& lex)
    {
        auto sp = parse_simplepath(lex);
        return ::HIR::GenericPath(mv$(sp), parse_params(lex));
    }
    HIR::Path parse_path(TokenStream& lex)
    {
        if( lex.lookahead(0) == TOK_LT || lex.lookahead(0) == TOK_DOUBLE_LT )
        {
            TODO(lex.point_span(), "parse_path - ufcs");
        }
        else
        {
            return parse_genericpath(lex);
        }
    }
    HIR::TypeRef parse_type(TokenStream& lex)
    {
        Token   tok;
        GET_TOK(tok, lex);
        switch( tok.type() )
        {
        case TOK_PAREN_OPEN: {
            ::std::vector<HIR::TypeRef> tys;
            while( lex.lookahead(0) != TOK_PAREN_CLOSE )
            {
                tys.push_back( parse_type(lex) );
                if( !consume_if(lex, TOK_COMMA) )
                    break;
            }
            GET_CHECK_TOK(tok, lex, TOK_PAREN_CLOSE);
            return HIR::TypeRef(mv$(tys));
            } break;
        case TOK_IDENT:
            if( tok.istr() == "dyn" )
            {
                TODO(lex.point_span(), tok);
            }
            else if( tok.istr() == "bool" ) return HIR::TypeRef(HIR::CoreType::Bool);
            else if( tok.istr() == "str" )  return HIR::TypeRef(HIR::CoreType::Str );
            else if( tok.istr() == "u8" ) return HIR::TypeRef(HIR::CoreType::U8);
            else if( tok.istr() == "i8" ) return HIR::TypeRef(HIR::CoreType::I8);
            else if( tok.istr() == "u16" ) return HIR::TypeRef(HIR::CoreType::U16);
            else if( tok.istr() == "i16" ) return HIR::TypeRef(HIR::CoreType::I16);
            else if( tok.istr() == "u32" ) return HIR::TypeRef(HIR::CoreType::U32);
            else if( tok.istr() == "i32" ) return HIR::TypeRef(HIR::CoreType::I32);
            else if( tok.istr() == "u64" ) return HIR::TypeRef(HIR::CoreType::U64);
            else if( tok.istr() == "i64" ) return HIR::TypeRef(HIR::CoreType::I64);
            else if( tok.istr() == "u128" ) return HIR::TypeRef(HIR::CoreType::U128);
            else if( tok.istr() == "i128" ) return HIR::TypeRef(HIR::CoreType::I128);
            else if( tok.istr() == "usize" ) return HIR::TypeRef(HIR::CoreType::Usize);
            else if( tok.istr() == "isize" ) return HIR::TypeRef(HIR::CoreType::Isize);
            else
            {
                TODO(lex.point_span(), tok);
            }
            break;
        case TOK_DOUBLE_AMP:
            lex.putback(TOK_AMP);
        case TOK_AMP:
            if( consume_if(lex, TOK_RWORD_MOVE) )
                return HIR::TypeRef::new_borrow(HIR::BorrowType::Owned, parse_type(lex));
            else if( consume_if(lex, TOK_RWORD_MUT) )
                return HIR::TypeRef::new_borrow(HIR::BorrowType::Unique, parse_type(lex));
            else
                return HIR::TypeRef::new_borrow(HIR::BorrowType::Shared, parse_type(lex));
        default:
            TODO(lex.point_span(), tok);
        }
    }
    MIR::LValue::Storage parse_lvalue_root(TokenStream& lex, const ::std::map<RcString, MIR::LValue::Storage>& name_map)
    {
        if( lex.lookahead(0) == TOK_DOUBLE_COLON )
        {
            // Static
            return MIR::LValue::Storage::new_Static( parse_path(lex) );
        }
        else
        {
            Token   tok;
            GET_TOK(tok, lex);
            CHECK_TOK(tok, TOK_IDENT);
            return name_map.at(tok.istr()).clone();
        }
    }
    MIR::LValue parse_lvalue(TokenStream& lex, const ::std::map<RcString, MIR::LValue::Storage>& name_map)
    {
        Token   tok;
        std::vector<MIR::LValue::Wrapper>   wrappers;
        auto root = parse_lvalue_root(lex, name_map);

        while(true)
        {
            GET_TOK(tok, lex);
            switch( tok.type() )
            {
            case TOK_SQUARE_OPEN:
                TODO(lex.point_span(), "parse_lvalue - indexing");
                break;
            case TOK_DOT:
                GET_CHECK_TOK(tok, lex, TOK_INTEGER);
                ASSERT_BUG(lex.point_span(), tok.intval() < UINT_MAX, "");
                wrappers.push_back(::MIR::LValue::Wrapper::new_Field( static_cast<unsigned>(tok.intval()) ));
                break;
            case TOK_HASH:
                GET_CHECK_TOK(tok, lex, TOK_INTEGER);
                ASSERT_BUG(lex.point_span(), tok.intval() < UINT_MAX, "");
                wrappers.push_back(::MIR::LValue::Wrapper::new_Downcast( static_cast<unsigned>(tok.intval()) ));
                break;
            case TOK_STAR:
                wrappers.push_back(::MIR::LValue::Wrapper::new_Deref());
                break;
            default:
                lex.putback(mv$(tok));
                return MIR::LValue(mv$(root), mv$(wrappers));
            }
        }
    }

    MIR::LValue parse_param(TokenStream& lex, const ::std::map<RcString, MIR::LValue::Storage>& name_map)
    {
        // Can be any constant, or an LValue
        return parse_lvalue(lex, name_map);
    }
}
